/*
 * metadesk-client — main entry point.
 * Receive → decode → display pipeline over TCP.
 *
 * Phase 1 flow:
 *   1. Connect TCP to host on port 7700
 *   2. Initialize H.264 decoder
 *   3. Create SDL2 window for display
 *   4. Main loop:
 *      a. Receive framed packets from host
 *      b. Decode H.264 video frames
 *      c. Display RGBA frames via SDL2
 *      d. Handle ping/pong keepalive
 *   5. Clean shutdown on SIGINT/SIGTERM or window close
 */
#include "fips_addr.h"
#include "session.h"
#include "packet.h"
#include "stream.h"
#include "decode.h"
#include "render.h"
#include "signer.h"
#include "nostr.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── State ───────────────────────────────────────────────────── */

typedef struct {
    MdRenderer   *renderer;
    MdOverlay    *overlay;
    uint32_t      frames_decoded;
    uint32_t      frames_displayed;
    int64_t       total_decode_us;

    /* Nostr session negotiation state */
    char          transport_addr[256]; /* FIPS addr from on_transport     */
    volatile int  transport_ready;     /* set by on_transport callback    */
    char          accepted_session_id[64]; /* from session_accept DM     */
    uint32_t      granted_caps;       /* from session_accept DM          */
    volatile int  session_accepted;   /* set by on_dm callback           */
} ClientCtx;

/* ── Decode callback: display frame ──────────────────────────── */

static void on_decoded(const MdDecodedFrame *frame, void *userdata) {
    ClientCtx *ctx = userdata;
    ctx->frames_decoded++;

    if (ctx->renderer) {
        int ret = md_renderer_present(ctx->renderer, frame->data,
                                      frame->width, frame->height);
        if (ret == 0)
            ctx->frames_displayed++;
    }

    /* Render overlay on top of the video frame */
    if (ctx->overlay) {
        md_overlay_new_frame(ctx->overlay);
        /* Stats are updated from the main loop, overlay_render called there */
    }
}

/* ── Nostr callbacks for --npub session negotiation ──────────── */

static void on_transport(const char *pubkey_hex, const char *fips_addr,
                         void *userdata) {
    ClientCtx *ctx = userdata;
    if (!ctx || !fips_addr) return;
    strncpy(ctx->transport_addr, fips_addr, sizeof(ctx->transport_addr) - 1);
    ctx->transport_addr[sizeof(ctx->transport_addr) - 1] = '\0';
    ctx->transport_ready = 1;
    (void)pubkey_hex;
}

static void on_session_dm(const char *sender_pubkey_hex, const char *content,
                          void *userdata) {
    ClientCtx *ctx = userdata;
    if (!ctx || !content) return;

    /* Try to parse as session_accept */
    MdSessionAccept acc;
    if (md_session_accept_from_json(content, &acc) == 0) {
        strncpy(ctx->accepted_session_id, acc.session_id,
                sizeof(ctx->accepted_session_id) - 1);
        ctx->granted_caps = acc.granted;
        ctx->session_accepted = 1;
    }
    (void)sender_pubkey_hex;
}

/* ── Usage ───────────────────────────────────────────────────── */

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s HOST|--npub NPUB [OPTIONS]\n\n", argv0);
    fprintf(stderr, "Connect to a metadesk host and display remote desktop.\n\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  HOST               Host address (IP or hostname)\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --npub NPUB        Connect via FIPS mesh (npub1xxx)\n");
    fprintf(stderr, "  --port PORT        Host port (default: %d)\n", MD_STREAM_PORT);
    fprintf(stderr, "  --no-display       Decode only, no SDL2 window\n");
    fprintf(stderr, "  --timeout MS       Connect timeout (default: 5000)\n");
    fprintf(stderr, "\nSigner options (choose one):\n");
    fprintf(stderr, "  --bunker URI       NIP-46 Nostr Connect bunker URI\n");
    fprintf(stderr, "  --dbus-signer      Use NIP-55L D-Bus signer daemon\n");
    fprintf(stderr, "  --socket-signer [PATH]  Use NIP-5F Unix socket signer\n");
    fprintf(stderr, "  --auto-signer      Auto-detect local signer\n");
    fprintf(stderr, "  --relay URL        Relay URL (default: wss://relay.sharegap.net)\n");
    fprintf(stderr, "  -h, --help         Show this help\n");
}

/* ── Timing helper ───────────────────────────────────────────── */

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Default options */
    const char *host = NULL;
    const char *npub = NULL;
    const char *bunker_uri = NULL;
    const char *socket_path = NULL;
    bool use_dbus_signer = false;
    bool auto_signer = false;
    uint16_t port = MD_STREAM_PORT;
    bool do_display = true;
    uint32_t timeout_ms = 5000;
    const char *relay_urls[16];
    int relay_count = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-display") == 0)
            do_display = false;
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout_ms = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--npub") == 0 && i + 1 < argc)
            npub = argv[++i];
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
        else if (argv[i][0] != '-' && !host)
            host = argv[i];
    }

    if (!host && !npub) {
        fprintf(stderr, "ERROR: host address or --npub required\n\n");
        usage(argv[0]);
        return 1;
    }

    printf("metadesk-client v0.1.0\n");
    if (npub) {
        printf("  fips:      %.*s...\n", 12, npub);
    } else {
        printf("  host:      %s:%u\n", host, port);
    }
    printf("  display:   %s\n", do_display ? "SDL2" : "disabled");
    printf("  timeout:   %u ms\n", timeout_ms);
    printf("\n");

    /* ── Signer initialization ─────────────────────────────── */
    MdSigner *signer = NULL;
    if (bunker_uri) {
        printf("client: connecting to NIP-46 bunker...\n");
        signer = md_signer_create_nip46(bunker_uri, 30000);
    } else if (use_dbus_signer) {
        printf("client: connecting to NIP-55L D-Bus signer...\n");
        signer = md_signer_create_nip55l();
    } else if (socket_path || auto_signer) {
        if (socket_path) {
            printf("client: connecting to NIP-5F socket signer...\n");
            signer = md_signer_create_nip5f(socket_path);
        }
        if (!signer && auto_signer) {
            printf("client: auto-detecting signer...\n");
            signer = md_signer_auto_detect();
        }
    }
    if (signer) {
        printf("client: signer ready (%s)\n",
               md_signer_type_name(md_signer_get_type(signer)));
    } else if (bunker_uri || use_dbus_signer) {
        fprintf(stderr, "ERROR: requested signer backend not available\n");
        return 1;
    }

    /* Initialize session state */
    MdSession session;
    md_session_init(&session);

    /* Client context (needed before connect for nostr callbacks) */
    ClientCtx ctx = { 0 };

    /* Connect to host */
    MdStream *stream = NULL;
    MdNostr *nostr = NULL;

    if (npub) {
        /* ── Nostr-signaled FIPS connect (Phase 2.1) ──────────── */

        /* Ensure we have a signer for NIP-17 DMs */
        if (!signer) {
            /* Generate ephemeral key if no signer configured */
            char *eph_sk = NULL, *eph_pk = NULL;
            if (md_nostr_generate_keypair(&eph_sk, &eph_pk) != 0) {
                fprintf(stderr, "ERROR: failed to generate ephemeral keypair\n");
                return 1;
            }
            signer = md_signer_create_direct(eph_sk);
            memset(eph_sk, 0, strlen(eph_sk));
            free(eph_sk);
            free(eph_pk);
            if (!signer) {
                fprintf(stderr, "ERROR: failed to create ephemeral signer\n");
                return 1;
            }
            printf("client: using ephemeral signer\n");
        }

        /* Default relay if none specified */
        if (relay_count == 0) {
            relay_urls[0] = "wss://relay.sharegap.net";
            relay_count = 1;
        }

        /* Transport + DM callbacks */
        MdNostrCallbacks nostr_cbs = { 0 };

        nostr_cbs.on_transport = on_transport;
        nostr_cbs.transport_userdata = &ctx;
        nostr_cbs.on_dm = on_session_dm;
        nostr_cbs.dm_userdata = &ctx;

        MdNostrConfig nostr_cfg = {
            .signer = signer,
            .relay_urls = relay_urls,
            .relay_count = relay_count,
        };

        nostr = md_nostr_create(&nostr_cfg, &nostr_cbs);
        if (!nostr) {
            fprintf(stderr, "ERROR: failed to create Nostr bridge\n");
            if (signer) md_signer_destroy(signer);
            return 1;
        }

        /* Step 1: Subscribe to host's transport address */
        printf("client: looking up transport addr for %.*s...\n", 12, npub);
        if (md_nostr_subscribe_transport(nostr, npub) != 0) {
            fprintf(stderr, "ERROR: failed to subscribe to transport\n");
            md_nostr_destroy(nostr);
            return 1;
        }

        /* Step 2: Wait for on_transport callback (with timeout) */
        int64_t deadline = now_us() + (int64_t)timeout_ms * 1000;
        while (!ctx.transport_ready && now_us() < deadline && g_running) {
            usleep(50000); /* 50ms poll */
        }
        if (!ctx.transport_ready) {
            fprintf(stderr, "ERROR: timed out waiting for host transport addr\n");
            md_nostr_destroy(nostr);
            return 1;
        }
        printf("client: found host transport addr: %s\n", ctx.transport_addr);

        /* Step 3: Send session request DM */
        MdSessionRequest req = {
            .capabilities = MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT,
            .tree_format = MD_TREE_FORMAT_COMPACT,
        };
        const char *our_pk = md_nostr_get_npub(nostr);
        if (our_pk)
            strncpy(req.fips_addr, our_pk, sizeof(req.fips_addr) - 1);

        char *req_json = md_session_request_to_json(&req);
        if (!req_json) {
            fprintf(stderr, "ERROR: failed to serialize session request\n");
            md_nostr_destroy(nostr);
            return 1;
        }

        printf("client: sending session request DM...\n");
        if (md_nostr_send_session_request(nostr, npub, req_json) != 0) {
            fprintf(stderr, "ERROR: failed to send session request\n");
            free(req_json);
            md_nostr_destroy(nostr);
            return 1;
        }
        free(req_json);

        /* Step 4: Wait for session accept DM */
        deadline = now_us() + (int64_t)timeout_ms * 2 * 1000;
        while (!ctx.session_accepted && now_us() < deadline && g_running) {
            usleep(50000); /* 50ms poll */
        }
        if (!ctx.session_accepted) {
            fprintf(stderr, "ERROR: timed out waiting for session accept\n");
            md_nostr_destroy(nostr);
            return 1;
        }
        printf("client: session accepted (id=%s)\n", ctx.accepted_session_id);

        /* Step 5: Connect via FIPS */
        printf("client: connecting via FIPS to %s...\n", ctx.transport_addr);
        stream = md_stream_connect_fips(npub, port, timeout_ms);
        if (!stream) {
            fprintf(stderr, "ERROR: FIPS connect failed\n");
            md_nostr_destroy(nostr);
            return 1;
        }

        /* Step 6: Send MD_PKT_SESSION_INFO with session_id + capabilities */
        MdSessionAccept acc;
        strncpy(acc.session_id, ctx.accepted_session_id, sizeof(acc.session_id) - 1);
        acc.granted = ctx.granted_caps;
        char *info_json = md_session_accept_to_json(&acc);
        if (info_json) {
            md_stream_send(stream, MD_PKT_SESSION_INFO, 0,
                           (const uint8_t *)info_json, (uint32_t)strlen(info_json));
            free(info_json);
        }

        /* Update session state */
        md_session_request(&session, npub, req.capabilities, req.tree_format);
        md_session_accept(&session, ctx.accepted_session_id, ctx.granted_caps);
        md_session_activate(&session);

    } else {
        /* ── Direct IP/hostname connect (existing path) ───────── */
        printf("client: connecting to %s:%u...\n", host, port);
        stream = md_stream_connect(host, port, timeout_ms);
        if (!stream) {
            fprintf(stderr, "ERROR: failed to connect to %s:%u\n", host, port);
            return 1;
        }
    }
    printf("client: connected\n");

    /* Create decoder */
    MdDecoder *decoder = md_decoder_create();
    if (!decoder) {
        fprintf(stderr, "ERROR: failed to create H.264 decoder\n");
        md_stream_destroy(stream);
        return 1;
    }

    /* Create renderer (SDL2 window) and overlay */
    if (do_display) {
        ctx.renderer = md_renderer_create(1920, 1080, "metadesk");
        if (!ctx.renderer) {
            fprintf(stderr, "WARNING: SDL2 renderer unavailable, decode-only mode\n");
        } else {
            /* Create Dear ImGui overlay on top of the renderer */
            ctx.overlay = md_overlay_create(
                md_renderer_get_sdl_window(ctx.renderer),
                md_renderer_get_sdl_renderer(ctx.renderer));
            if (!ctx.overlay) {
                fprintf(stderr, "WARNING: ImGui overlay unavailable\n");
            }
        }
    }

    /* ── Main loop ───────────────────────────────────────────── */
    printf("client: receiving stream... (Ctrl+C to stop)\n\n");

    uint32_t pkt_seq = 0;
    uint32_t last_stats_ms = md_stream_now_ms();
    uint32_t video_packets = 0;

    while (g_running && md_stream_is_connected(stream)) {
        /* Check SDL events (window close, etc.) */
        if (ctx.renderer) {
            if (md_renderer_poll_events(ctx.renderer) < 0) {
                printf("client: window closed\n");
                break;
            }
        }

        /* Receive next packet */
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        int ret = md_stream_recv(stream, &hdr, &payload, 16 /* ~1 frame at 60fps */);

        if (ret == 1) {
            /* Timeout — no data yet, loop back */
            continue;
        }
        if (ret < 0) {
            /* Connection lost */
            fprintf(stderr, "client: connection lost\n");
            break;
        }

        /* Handle packet by type */
        switch (hdr.type) {
        case MD_PKT_VIDEO_FRAME: {
            video_packets++;

            int64_t dec_start = now_us();

            ret = md_decoder_submit(decoder, payload, hdr.payload_len, hdr.sequence);
            if (ret == 0) {
                md_decoder_poll(decoder, on_decoded, &ctx);
            }

            int64_t dec_elapsed = now_us() - dec_start;
            ctx.total_decode_us += dec_elapsed;
            break;
        }

        case MD_PKT_PING:
            /* Reply with pong */
            md_stream_send(stream, MD_PKT_PONG, pkt_seq++, NULL, 0);
            break;

        case MD_PKT_PONG:
            md_stream_handle_pong(stream, &hdr);
            break;

        case MD_PKT_UI_TREE:
        case MD_PKT_UI_TREE_DELTA:
            /* TODO: parse and display UI tree (M1.6/M1.7) */
            break;

        case MD_PKT_SESSION_INFO:
            fprintf(stderr, "client: received session info\n");
            break;

        default:
            fprintf(stderr, "client: unknown packet type 0x%02x\n", hdr.type);
            break;
        }

        free(payload);

        /* Update and render overlay */
        if (ctx.overlay) {
            MdStreamStats stream_stats;
            md_stream_get_stats(stream, &stream_stats);

            double avg_decode_ms = ctx.frames_decoded > 0
                ? (double)ctx.total_decode_us / ctx.frames_decoded / 1000.0
                : 0.0;

            MdOverlayStats overlay_stats = {
                .latency_ms   = (float)(avg_decode_ms + stream_stats.avg_rtt_ms),
                .encode_ms    = 0.0f,  /* TODO: receive from host stats */
                .decode_ms    = (float)avg_decode_ms,
                .rtt_ms       = (float)stream_stats.avg_rtt_ms,
                .connected    = md_stream_is_connected(stream),
                .fps          = ctx.frames_decoded > 0
                    ? (int)(ctx.frames_decoded * 1000.0 /
                            (md_stream_now_ms() - last_stats_ms + 1)) : 0,
                .bitrate_mbps = stream_stats.bytes_recv > 0
                    ? (float)(stream_stats.bytes_recv * 8.0 / 1000000.0) : 0.0f,
                .encoder_name = NULL,
            };

            md_overlay_new_frame(ctx.overlay);
            md_overlay_render(ctx.overlay, &overlay_stats);
        }

        /* Print stats every 5 seconds */
        uint32_t now = md_stream_now_ms();
        if (now - last_stats_ms >= 5000) {
            MdStreamStats stats;
            md_stream_get_stats(stream, &stats);

            double avg_decode_ms = ctx.frames_decoded > 0
                ? (double)ctx.total_decode_us / ctx.frames_decoded / 1000.0
                : 0.0;

            printf("client: packets=%u decoded=%u displayed=%u "
                   "decode_avg=%.1fms rtt=%ums rx=%.1fMB\n",
                   video_packets, ctx.frames_decoded, ctx.frames_displayed,
                   avg_decode_ms,
                   stats.avg_rtt_ms,
                   (double)stats.bytes_recv / (1024.0 * 1024.0));

            last_stats_ms = now;
        }
    }

    /* ── Shutdown ────────────────────────────────────────────── */
    printf("\nclient: shutting down...\n");

    md_decoder_flush(decoder, on_decoded, &ctx);
    md_decoder_destroy(decoder);
    md_stream_destroy(stream);

    if (nostr)
        md_nostr_destroy(nostr);
    else if (signer)
        md_signer_destroy(signer); /* only if nostr didn't borrow it */
    if (ctx.overlay)
        md_overlay_destroy(ctx.overlay);
    if (ctx.renderer)
        md_renderer_destroy(ctx.renderer);

    printf("client: done. decoded %u frames, displayed %u\n",
           ctx.frames_decoded, ctx.frames_displayed);
    return 0;
}
