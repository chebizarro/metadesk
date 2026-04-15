/*
 * metadesk-host — main entry point.
 * Capture → encode → stream pipeline over TCP.
 *
 * Phase 1 flow:
 *   1. Start TCP server on port 7700
 *   2. Wait for client connection
 *   3. Initialize PipeWire capture and NVENC encoder
 *   4. For each captured frame:
 *      a. Encode to H.264 via FFmpeg
 *      b. Send encoded packet over TCP with MdPacketHeader framing
 *   5. Handle ping/pong keepalive
 *   6. Clean shutdown on SIGINT/SIGTERM
 */
#include "capture.h"
#include "encode.h"
#include "a11y.h"
#include "input.h"
#include "session.h"
#include "packet.h"
#include "stream.h"
#include "agent.h"
#include "fips_addr.h"
#include "nostr.h"
#include "secrets.h"
#include "signer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── State shared between capture callback and main loop ─────── */

typedef struct {
    MdEncoder    *encoder;
    MdStream     *client;
    MdAgent      *agent;
    MdNostr      *nostr;
    MdSession    *session;
    uint32_t      frame_seq;
    uint32_t      pkt_seq;
    int64_t       total_encode_us;
    uint32_t      frames_encoded;
    uint32_t      frames_sent;

    /* Nostr session negotiation */
    char          pending_client_pk[128]; /* pubkey of requesting client */
    MdSessionRequest pending_req;         /* parsed session request      */
    volatile int  session_requested;      /* set by on_dm callback       */
} HostCtx;

/* ── Encode callback: send encoded packet to client ──────────── */

static void on_encoded(const MdEncodedPacket *pkt, void *userdata) {
    HostCtx *ctx = userdata;
    if (!ctx->client || !md_stream_is_connected(ctx->client))
        return;

    int ret = md_stream_send(ctx->client, MD_PKT_VIDEO_FRAME,
                             ctx->pkt_seq++,
                             pkt->data, (uint32_t)pkt->size);
    if (ret == 0)
        ctx->frames_sent++;
}

/* ── Nostr callbacks for session negotiation ──────────────── */

static void host_on_dm(const char *sender_pubkey_hex, const char *content,
                       void *userdata) {
    HostCtx *ctx = userdata;
    if (!ctx || !content || ctx->session_requested) return;

    /* Try to parse as session_request */
    MdSessionRequest req;
    if (md_session_request_from_json(content, &req) == 0) {
        /* Check allowlist if available */
        if (ctx->nostr && !md_nostr_is_allowed(ctx->nostr, sender_pubkey_hex)) {
            fprintf(stderr, "host: session request from non-allowlisted %.*s... (accepting for Phase 2.1)\n",
                    8, sender_pubkey_hex);
            /* Phase 2.1: accept anyway; Phase 2.3 will enforce */
        }

        strncpy(ctx->pending_client_pk, sender_pubkey_hex,
                sizeof(ctx->pending_client_pk) - 1);
        ctx->pending_req = req;
        ctx->session_requested = 1;
        fprintf(stderr, "host: received session request from %.*s...\n",
                8, sender_pubkey_hex);
    }
}

/* ── Map capture pixel format to encoder pixel format ──────────── */

static MdPixFmt capture_fmt_to_enc(MdCapturePixFmt cfmt) {
    switch (cfmt) {
    case MD_PIX_CAPTURE_BGRA: return MD_PIX_FMT_BGRA;
    case MD_PIX_CAPTURE_NV12: return MD_PIX_FMT_NV12;
    default:                  return MD_PIX_FMT_BGRX;
    }
}

/* ── Capture thread: pull frames and encode ──────────────────── */

typedef struct {
    HostCtx      *host;
    MdCaptureCtx *capture;
} CaptureThread;

static void *capture_thread_func(void *arg) {
    CaptureThread *ct  = arg;
    HostCtx       *ctx = ct->host;
    MdCaptureCtx  *cap = ct->capture;

    while (md_capture_is_active(cap) && g_running) {
        MdFrame frame;
        if (md_capture_get_frame(cap, &frame) != 0)
            break;

        if (!ctx->encoder || !ctx->client) {
            md_capture_release_frame(cap, &frame);
            continue;
        }

        MdPixFmt fmt = capture_fmt_to_enc(frame.format);

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t start_us = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;

        int ret = md_encoder_submit(ctx->encoder, frame.data,
                                    frame.stride, fmt,
                                    (int64_t)frame.seq,
                                    on_encoded, ctx);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t elapsed_us = (ts.tv_sec * 1000000LL + ts.tv_nsec / 1000) - start_us;

        if (ret == 0) {
            ctx->total_encode_us += elapsed_us;
            ctx->frames_encoded++;
        }
        ctx->frame_seq++;

        md_capture_release_frame(cap, &frame);
    }
    return NULL;
}

/* ── Handle incoming packets from client ─────────────────────── */

static void handle_client_packet(HostCtx *ctx, const MdPacketHeader *hdr,
                                 const uint8_t *payload) {
    switch (hdr->type) {
    case MD_PKT_PING:
        /* Reply with pong */
        md_stream_send(ctx->client, MD_PKT_PONG, ctx->pkt_seq++, NULL, 0);
        break;

    case MD_PKT_PONG:
        md_stream_handle_pong(ctx->client, hdr);
        break;

    case MD_PKT_ACTION:
        if (ctx->agent) {
            md_agent_handle_action(ctx->agent, ctx->client, &ctx->pkt_seq,
                                   payload, hdr->payload_len);
        } else {
            fprintf(stderr, "host: received action but no agent handler\n");
        }
        break;

    case MD_PKT_SESSION_INFO: {
        fprintf(stderr, "host: received session info\n");
        /* Parse session info and ack with granted capabilities */
        if (payload && hdr->payload_len > 0) {
            MdSessionAccept acc;
            char json_buf[4096];
            size_t len = hdr->payload_len < sizeof(json_buf) - 1
                         ? hdr->payload_len : sizeof(json_buf) - 1;
            memcpy(json_buf, payload, len);
            json_buf[len] = '\0';
            if (md_session_accept_from_json(json_buf, &acc) == 0) {
                fprintf(stderr, "host: session_id=%s\n", acc.session_id);
                if (ctx->session)
                    md_session_activate(ctx->session);
            }
        }
        /* Echo back as ack */
        md_stream_send(ctx->client, MD_PKT_SESSION_INFO, ctx->pkt_seq++,
                       payload, hdr->payload_len);
        break;
    }

    default:
        fprintf(stderr, "host: unknown packet type 0x%02x\n", hdr->type);
        break;
    }
}

/* ── Usage ───────────────────────────────────────────────────── */

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n\n", argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --port PORT      Listen port (default: %d)\n", MD_STREAM_PORT);
    fprintf(stderr, "  --bind ADDR      Bind address (default: any)\n");
    fprintf(stderr, "  --fps FPS        Target framerate (default: 60)\n");
    fprintf(stderr, "  --bitrate BPS    Encoder bitrate (default: 8000000)\n");
    fprintf(stderr, "  --no-nvenc       Disable NVENC, use x264\n");
    fprintf(stderr, "  --no-capture     Skip screen capture (test mode)\n");
    fprintf(stderr, "  --npub NPUB      Require FIPS client npub for auth\n");
    fprintf(stderr, "\nSigner options (choose one):\n");
    fprintf(stderr, "  --bunker URI     NIP-46 Nostr Connect bunker URI\n");
    fprintf(stderr, "  --dbus-signer    Use NIP-55L D-Bus signer daemon\n");
    fprintf(stderr, "  --socket-signer [PATH]  Use NIP-5F Unix socket signer\n");
    fprintf(stderr, "  --auto-signer    Auto-detect local signer (NIP-5F, NIP-55L)\n");
    fprintf(stderr, "  --relay URL      Relay URL (default: wss://relay.sharegap.net)\n");
    fprintf(stderr, "  -h, --help       Show this help\n");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Default options */
    uint16_t port = MD_STREAM_PORT;
    const char *bind_addr = NULL;
    const char *fips_npub = NULL;  /* expected client npub (FIPS auth) */
    const char *bunker_uri = NULL;
    const char *socket_path = NULL;
    bool use_dbus_signer = false;
    bool auto_signer = false;
    uint32_t fps = 60;
    uint32_t bitrate = MD_ENCODER_DEFAULT_BITRATE;
    bool use_nvenc = true;
    bool do_capture = true;
    const char *relay_urls[16];
    int relay_count = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc)
            bind_addr = argv[++i];
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
            fps = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc)
            bitrate = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-nvenc") == 0)
            use_nvenc = false;
        else if (strcmp(argv[i], "--no-capture") == 0)
            do_capture = false;
        else if (strcmp(argv[i], "--npub") == 0 && i + 1 < argc)
            fips_npub = argv[++i];
        else if (strcmp(argv[i], "--bunker") == 0 && i + 1 < argc)
            bunker_uri = argv[++i];
        else if (strcmp(argv[i], "--dbus-signer") == 0)
            use_dbus_signer = true;
        else if (strcmp(argv[i], "--socket-signer") == 0) {
            socket_path = (i + 1 < argc && argv[i + 1][0] != '-')
                ? argv[++i] : NULL;
        }
        else if (strcmp(argv[i], "--auto-signer") == 0)
            auto_signer = true;
        else if (strcmp(argv[i], "--relay") == 0 && i + 1 < argc) {
            if (relay_count < 16)
                relay_urls[relay_count++] = argv[++i];
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    printf("metadesk-host v0.1.0\n");
    printf("  port:      %u\n", port);
    printf("  fps:       %u\n", fps);
    printf("  bitrate:   %u bps\n", bitrate);
    printf("  encoder:   %s\n", use_nvenc ? "NVENC (preferred)" : "x264");
    printf("  capture:   %s\n", do_capture ? "enabled" : "disabled");
    if (fips_npub)
        printf("  fips:      %.*s...\n", 12, fips_npub);
    printf("\n");

    /* ── Signer initialization ────────────────────────────────── */
    MdSigner *signer = NULL;
    if (bunker_uri) {
        printf("host: connecting to NIP-46 bunker...\n");
        signer = md_signer_create_nip46(bunker_uri, 30000);
    } else if (use_dbus_signer) {
        printf("host: connecting to NIP-55L D-Bus signer...\n");
        signer = md_signer_create_nip55l();
    } else if (socket_path || auto_signer) {
        if (socket_path) {
            printf("host: connecting to NIP-5F socket signer...\n");
            signer = md_signer_create_nip5f(socket_path);
        }
        if (!signer && auto_signer) {
            printf("host: auto-detecting signer...\n");
            signer = md_signer_auto_detect();
        }
    }
    /* If no signer from CLI, log that Nostr features require a signer */
    if (signer) {
        printf("host: signer ready (%s)\n",
               md_signer_type_name(md_signer_get_type(signer)));
    } else if (bunker_uri || use_dbus_signer) {
        fprintf(stderr, "ERROR: requested signer backend not available\n");
        return 1;
    }

    /* If FIPS npub specified, bind to FIPS address */
    if (fips_npub && !bind_addr) {
        printf("host: FIPS mode — accepting connections via fips0 TUN\n");
    }

    /* Initialize session state */
    MdSession session;
    md_session_init(&session);

    /* Host context (needed before TCP for nostr callbacks) */
    HostCtx ctx = {
        .session = &session,
    };

    /* ── Nostr bridge (if signer available) ───────────────── */
    MdNostr *nostr = NULL;
    if (signer) {
        if (relay_count == 0) {
            relay_urls[0] = "wss://relay.sharegap.net";
            relay_count = 1;
        }

        MdNostrCallbacks nostr_cbs = { 0 };
        nostr_cbs.on_dm = host_on_dm;
        nostr_cbs.dm_userdata = &ctx;

        MdNostrConfig nostr_cfg = {
            .signer = signer,
            .relay_urls = relay_urls,
            .relay_count = relay_count,
        };

        nostr = md_nostr_create(&nostr_cfg, &nostr_cbs);
        if (nostr) {
            ctx.nostr = nostr;

            /* Publish our transport address */
            char *our_pk = NULL;
            if (md_signer_get_pubkey(signer, &our_pk) == MD_SIGNER_OK && our_pk) {
                /* Use pubkey as FIPS address placeholder */
                md_nostr_publish_transport(nostr, our_pk);
                free(our_pk);
            }

            /* Subscribe to allowlist updates */
            md_nostr_refresh_allowlist(nostr);

            printf("host: nostr bridge ready, waiting for session requests...\n");

            /* Wait for a session request DM (with timeout) */
            int wait_count = 0;
            while (!ctx.session_requested && g_running && wait_count < 600) {
                usleep(100000); /* 100ms poll */
                wait_count++;
            }

            if (ctx.session_requested) {
                /* Generate session ID */
                char session_id[64];
                snprintf(session_id, sizeof(session_id), "%08x-%04x-%04x",
                         (uint32_t)time(NULL), (uint32_t)(rand() & 0xFFFF),
                         (uint32_t)(rand() & 0xFFFF));

                /* Determine granted capabilities */
                uint32_t granted = ctx.pending_req.capabilities &
                                   (MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT);

                /* Send session accept DM */
                MdSessionAccept acc = { .granted = granted };
                strncpy(acc.session_id, session_id, sizeof(acc.session_id) - 1);

                char *acc_json = md_session_accept_to_json(&acc);
                if (acc_json) {
                    md_nostr_send_session_accept(nostr, ctx.pending_client_pk,
                                                acc_json);
                    free(acc_json);
                    printf("host: sent session accept to %.*s... (session=%s)\n",
                           8, ctx.pending_client_pk, session_id);
                }

                /* Update session state */
                md_session_request(&session, ctx.pending_client_pk,
                                   ctx.pending_req.capabilities,
                                   ctx.pending_req.tree_format);
                md_session_accept(&session, session_id, granted);
            } else if (g_running) {
                printf("host: no session request received, accepting direct TCP\n");
            }
        } else {
            fprintf(stderr, "WARNING: nostr bridge creation failed, direct TCP only\n");
        }
    }

    /* Create TCP server */
    MdStreamServer *srv = md_stream_server_create(bind_addr, port);
    if (!srv) {
        fprintf(stderr, "ERROR: failed to bind TCP server on port %u\n", port);
        return 1;
    }
    printf("host: listening on port %u\n", port);

    /* Wait for client connection */
    printf("host: waiting for client...\n");
    MdStream *client = NULL;
    while (g_running && !client) {
        client = md_stream_server_accept(srv, 1000);
    }
    if (!client || !g_running) {
        md_stream_server_destroy(srv);
        return g_running ? 1 : 0;
    }
    printf("host: client connected\n");

    /* Initialize encoder */
    /* Note: dimensions are set after first capture frame. For Phase 1,
     * we use a default 1920x1080 and resize if needed. */
    MdEncoderConfig enc_cfg = {
        .width        = 1920,
        .height       = 1080,
        .bitrate      = bitrate,
        .fps          = fps,
        .prefer_nvenc = use_nvenc,
    };

    MdEncoder *encoder = md_encoder_create(&enc_cfg);
    if (!encoder) {
        fprintf(stderr, "ERROR: failed to create encoder\n");
        md_stream_destroy(client);
        md_stream_server_destroy(srv);
        return 1;
    }
    printf("host: encoder ready (%s)\n",
           md_encoder_is_hw(encoder) ? "NVENC" : "x264");

    /* Initialize input injection */
    MdInputConfig input_cfg = {
        .screen_width  = 1920,
        .screen_height = 1080,
    };
    MdInput *input = md_input_create(&input_cfg);
    if (!input || !md_input_is_ready(input)) {
        fprintf(stderr, "WARNING: input injection unavailable\n");
    } else {
        printf("host: input injection ready\n");
    }

    /* Initialize accessibility tree walker */
    MdA11yCtx *a11y = md_a11y_create();
    if (!a11y) {
        fprintf(stderr, "WARNING: accessibility tree unavailable — agent mode disabled\n");
    } else {
        printf("host: accessibility tree connected\n");
    }

    /* Initialize agent action handler */
    MdAgentConfig agent_cfg = {
        .a11y        = a11y,
        .input       = input,
        .tree_format = session.tree_format,
        .settle_ms   = MD_AGENT_DEFAULT_SETTLE_MS,
    };
    MdAgent *agent = md_agent_create(&agent_cfg);

    /* Set up host context (update fields set after TCP connect) */
    ctx.encoder = encoder;
    ctx.client  = client;
    ctx.agent   = agent;

    /* Send initial UI tree to client */
    if (agent && a11y) {
        md_agent_send_tree(agent, client, &ctx.pkt_seq);
        printf("host: sent initial UI tree\n");
    }

    /* Start screen capture */
    MdCaptureCtx *capture = NULL;
    pthread_t cap_thread = 0;
    bool cap_thread_started = false;
    CaptureThread cap_thr_ctx = {0};
    if (do_capture) {
        MdCaptureConfig cap_cfg = {
            .target_fps  = fps,
            .show_cursor = true,
        };
        capture = md_capture_create(&cap_cfg);
        if (!capture) {
            fprintf(stderr, "WARNING: screen capture unavailable\n");
        } else {
            if (md_capture_start(capture) < 0) {
                fprintf(stderr, "WARNING: failed to start capture\n");
                md_capture_destroy(capture);
                capture = NULL;
            } else {
                printf("host: capture started\n");
                /* Spawn a thread to pull frames and encode */
                cap_thr_ctx.host    = &ctx;
                cap_thr_ctx.capture = capture;
                if (pthread_create(&cap_thread, NULL, capture_thread_func, &cap_thr_ctx) == 0)
                    cap_thread_started = true;
            }
        }
    }

    /* ── Main loop ───────────────────────────────────────────── */
    printf("host: streaming... (Ctrl+C to stop)\n\n");

    uint32_t last_stats_ms = md_stream_now_ms();
    uint32_t last_ping_ms = last_stats_ms;

    while (g_running && md_stream_is_connected(client)) {
        /* Check for incoming packets from client (non-blocking) */
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        int ret = md_stream_recv(client, &hdr, &payload, 10 /* 10ms poll */);

        if (ret == 0) {
            handle_client_packet(&ctx, &hdr, payload);
            free(payload);
        } else if (ret < 0) {
            /* Connection lost */
            break;
        }
        /* ret == 1: timeout, no data — continue */

        uint32_t now = md_stream_now_ms();

        /* Send periodic ping for RTT measurement */
        if (now - last_ping_ms >= 1000) {
            md_stream_send_ping(client);
            last_ping_ms = now;
        }

        /* Print stats every 5 seconds */
        if (now - last_stats_ms >= 5000) {
            MdStreamStats stats;
            md_stream_get_stats(client, &stats);

            double avg_encode_ms = ctx.frames_encoded > 0
                ? (double)ctx.total_encode_us / ctx.frames_encoded / 1000.0
                : 0.0;

            printf("host: frames=%u sent=%u encode_avg=%.1fms "
                   "rtt=%ums rtt_avg=%ums tx=%.1fMB rx=%.1fKB\n",
                   ctx.frames_encoded, ctx.frames_sent,
                   avg_encode_ms,
                   stats.last_rtt_ms, stats.avg_rtt_ms,
                   (double)stats.bytes_sent / (1024.0 * 1024.0),
                   (double)stats.bytes_recv / 1024.0);

            last_stats_ms = now;
        }
    }

    /* ── Shutdown ────────────────────────────────────────────── */
    printf("\nhost: shutting down...\n");

    if (capture) {
        md_capture_stop(capture);
        if (cap_thread_started)
            pthread_join(cap_thread, NULL);
        md_capture_destroy(capture);
    }

    md_encoder_flush(encoder, on_encoded, &ctx);
    md_encoder_destroy(encoder);

    if (agent) {
        printf("host: handled %u agent actions\n", md_agent_get_action_count(agent));
        md_agent_destroy(agent);
    }
    if (a11y)
        md_a11y_destroy(a11y);
    if (input)
        md_input_destroy(input);

    md_stream_destroy(client);
    md_stream_server_destroy(srv);

    if (nostr)
        md_nostr_destroy(nostr);
    else if (signer)
        md_signer_destroy(signer);

    printf("host: done. sent %u frames in %u packets\n",
           ctx.frames_encoded, ctx.frames_sent);
    return 0;
}
