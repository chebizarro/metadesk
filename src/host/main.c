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
#include "fips_control.h"
#include "nostr.h"
#include "secrets.h"
#include "signer.h"
#include "mcp_bridge.h"
#include "mcp_http.h"
#ifdef MD_ENABLE_FIPSNAT
#include "ipc.h"
#include "fipsnat_ipc.h"
#endif
#include "bitrate_ctrl.h"
#include "session_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <go.h>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ── State shared between capture callback and main loop ─────── */

typedef struct {
    MdEncoder    *encoder;
    MdStream     *client;
    pthread_mutex_t client_mu;  /* protects client pointer lifetime */
    MdAgent      *agent;
    MdNostr      *nostr;
    MdSession    *session;
    uint32_t      frame_seq;
    uint32_t      pkt_seq;
    int64_t       total_encode_us;
    uint32_t      frames_encoded;
    uint32_t      frames_sent;

    /* Session log for agent monitoring (M2.4) */
    MdSessionLog *session_log;

    /* Nostr session negotiation */
    char          expected_client_pk[128]; /* optional required client pubkey hex */
    char          pending_client_pk[128]; /* pubkey of requesting client */
    MdSessionRequest pending_req;         /* parsed session request      */
    volatile int  session_requested;      /* set by on_dm callback       */
    GoChannel      *session_req_ch;       /* signaled on session request */
} HostCtx;

/* ── Encode callback: send encoded packet to client ──────────── */

static void on_encoded(const MdEncodedPacket *pkt, void *userdata) {
    HostCtx *ctx = userdata;

    pthread_mutex_lock(&ctx->client_mu);
    MdStream *client = ctx->client;
    if (!client || !md_stream_is_connected(client)) {
        pthread_mutex_unlock(&ctx->client_mu);
        return;
    }

    int ret = md_stream_send(client, MD_PKT_VIDEO_FRAME,
                             ctx->pkt_seq++,
                             pkt->data, (uint32_t)pkt->size);
    if (ret == 0)
        ctx->frames_sent++;
    pthread_mutex_unlock(&ctx->client_mu);
}

/* ── Nostr callbacks for session negotiation ──────────────── */

static void host_on_dm(const char *sender_pubkey_hex, const char *content,
                       void *userdata) {
    HostCtx *ctx = userdata;
    if (!ctx || !content || ctx->session_requested) return;

    /* Try to parse as session_request */
    MdSessionRequest req;
    if (md_session_request_from_json(content, &req) == 0) {
        if (ctx->expected_client_pk[0] != '\0' &&
            strcmp(sender_pubkey_hex, ctx->expected_client_pk) != 0) {
            fprintf(stderr, "host: REJECTED session request from unexpected FIPS client %.*s...\n",
                    8, sender_pubkey_hex);
            return;
        }

        /* Enforce allowlist: if an allowlist is configured, reject
         * clients that are not on it. If no allowlist is configured
         * (open mode), accept all clients. */
        if (ctx->nostr && md_nostr_has_allowlist(ctx->nostr)
            && !md_nostr_is_allowed(ctx->nostr, sender_pubkey_hex)) {
            fprintf(stderr, "host: REJECTED session request from "
                    "non-allowlisted pubkey %.*s...\n",
                    8, sender_pubkey_hex);
            return;
        }

        strncpy(ctx->pending_client_pk, sender_pubkey_hex,
                sizeof(ctx->pending_client_pk) - 1);
        ctx->pending_req = req;
        ctx->session_requested = 1;
        go_channel_try_send(ctx->session_req_ch, (void *)(uintptr_t)1);
        fprintf(stderr, "host: received session request from %.*s...\n",
                8, sender_pubkey_hex);

        /* Log the request event */
        if (ctx->session_log)
            md_session_log_event(ctx->session_log, MD_SESSION_LOG_REQUEST,
                                 NULL, sender_pubkey_hex,
                                 "session request via NIP-17 DM");
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

        pthread_mutex_lock(&ctx->client_mu);
        bool has_client = (ctx->client != NULL);
        pthread_mutex_unlock(&ctx->client_mu);

        if (!ctx->encoder || !has_client) {
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
    fprintf(stderr, "  --fips-control PATH  FIPS daemon control socket override\n");
    fprintf(stderr, "  --fips-ready-timeout MS  Peer readiness timeout (default: 10000)\n");
    fprintf(stderr, "\nSigner options (choose one):\n");
    fprintf(stderr, "  --bunker URI     NIP-46 Nostr Connect bunker URI\n");
    fprintf(stderr, "  --dbus-signer    Use NIP-55L D-Bus signer daemon\n");
    fprintf(stderr, "  --socket-signer [PATH]  Use NIP-5F Unix socket signer\n");
    fprintf(stderr, "  --auto-signer    Auto-detect local signer (NIP-5F, NIP-55L)\n");
    fprintf(stderr, "  --relay URL      Relay URL (default: wss://relay.sharegap.net)\n");
    fprintf(stderr, "\nMCP agent interface:\n");
    fprintf(stderr, "  --mcp            Start MCP server on stdio (JSON-RPC 2.0)\n");
    fprintf(stderr, "  --mcp-http [PORT] Start MCP HTTP+SSE server (default: 7710)\n");
#ifdef MD_ENABLE_FIPSNAT
    fprintf(stderr, "\nLegacy NAT traversal (deprecated, compile-time opt-in):\n");
    fprintf(stderr, "  --fips-nat [NAME] Connect to legacy fips-nat IPC (not recommended; default: fips-nat)\n");
#endif
    fprintf(stderr, "  -h, --help       Show this help\n");
}


static void host_print_fips_setup_guidance(const char *npub,
                                           const MdFipsPeerReadiness *ready) {
    fprintf(stderr,
            "ERROR: host: FIPS peer not configured or not discovered by local daemon (%.*s...)\n",
            12, npub ? npub : "");
    if (ready && ready->detail[0])
        fprintf(stderr, "  FIPS detail: %s\n", ready->detail);
    fprintf(stderr,
            "  Configure this client peer in the FIPS daemon (via_nostr/auto_connect/peer discovery)\n"
            "  and verify `fipsctl show peers` reports a connected peer before using metadesk-host --npub.\n");
}

static int host_check_fips_daemon(const char *socket_path, uint32_t timeout_ms) {
    MdFipsControlResponse resp;
    md_fips_control_response_init(&resp);
    MdFipsControlResult r = md_fips_control_request(socket_path, "show_status",
                                                    NULL, timeout_ms, &resp);
    if (r != MD_FIPS_CONTROL_OK) {
        fprintf(stderr, "ERROR: FIPS daemon health check failed: %s%s%s\n",
                md_fips_control_result_string(r),
                resp.message ? ": " : "",
                resp.message ? resp.message : "");
        if (resp.socket_path)
            fprintf(stderr, "  control socket: %s\n", resp.socket_path);
        fprintf(stderr,
                "  Start and configure the FIPS daemon, or pass --fips-control PATH if it uses a custom socket.\n");
        md_fips_control_response_free(&resp);
        return -1;
    }

    cJSON *state = resp.data ? cJSON_GetObjectItemCaseSensitive(resp.data, "state") : NULL;
    cJSON *npub = resp.data ? cJSON_GetObjectItemCaseSensitive(resp.data, "npub") : NULL;
    printf("host: FIPS daemon ready");
    if (resp.socket_path) printf(" (%s)", resp.socket_path);
    if (cJSON_IsString(state) && state->valuestring) printf(" state=%s", state->valuestring);
    if (cJSON_IsString(npub) && npub->valuestring) printf(" npub=%.*s...", 12, npub->valuestring);
    printf("\n");
    md_fips_control_response_free(&resp);
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    install_signal_handlers();

    /* Default options */
    uint16_t port = MD_STREAM_PORT;
    const char *bind_addr = NULL;
    const char *fips_npub = NULL;  /* expected client npub (FIPS auth) */
    const char *bunker_uri = NULL;
    const char *socket_path = NULL;
    const char *fips_control_socket = NULL;
    bool use_dbus_signer = false;
    bool auto_signer = false;
    uint32_t fps = 60;
    uint32_t bitrate = MD_ENCODER_DEFAULT_BITRATE;
    bool use_nvenc = true;
    bool do_capture = true;
    bool mcp_stdio = false;
    bool mcp_http = false;
    uint16_t mcp_http_port = 0;  /* 0 = default (7710) */
    uint32_t fips_ready_timeout_ms = 10000;
#ifdef MD_ENABLE_FIPSNAT
    bool use_fipsnat = false;
    const char *fipsnat_ipc_name = "fips-nat";
#endif
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
        else if (strcmp(argv[i], "--fips-control") == 0 && i + 1 < argc)
            fips_control_socket = argv[++i];
        else if (strcmp(argv[i], "--fips-ready-timeout") == 0 && i + 1 < argc)
            fips_ready_timeout_ms = (uint32_t)atoi(argv[++i]);
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
        else if (strcmp(argv[i], "--mcp") == 0)
            mcp_stdio = true;
        else if (strcmp(argv[i], "--mcp-http") == 0) {
            mcp_http = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                mcp_http_port = (uint16_t)atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--fips-nat") == 0) {
#ifdef MD_ENABLE_FIPSNAT
            use_fipsnat = true;
            fprintf(stderr, "host: WARNING: --fips-nat is deprecated; prefer the FIPS daemon control path\n");
            if (i + 1 < argc && argv[i + 1][0] != '-')
                fipsnat_ipc_name = argv[++i];
#else
            fprintf(stderr, "ERROR: --fips-nat is deprecated and disabled in this build\n");
            fprintf(stderr, "  Rebuild with MD_ENABLE_FIPSNAT only for legacy NAT IPC testing.\n");
            return 1;
#endif
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
    if (fips_npub && !signer) {
        fprintf(stderr, "ERROR: host --npub requires a signer for Nostr session authorization\n");
        fprintf(stderr, "  Use --auto-signer, --socket-signer, --dbus-signer, or --bunker.\n");
        return 1;
    }

    char expected_client_pk[65] = {0};
    if (fips_npub) {
        if (md_fips_npub_to_pubkey_hex(fips_npub, expected_client_pk,
                                       sizeof(expected_client_pk)) != 0) {
            fprintf(stderr, "ERROR: --npub must be a valid bech32 FIPS/Nostr npub identity\n");
            if (signer) md_signer_destroy(signer);
            return 1;
        }

        if (!bind_addr)
            printf("host: FIPS mode — accepting connections via fips0 TUN\n");

        printf("host: checking FIPS daemon health...\n");
        if (host_check_fips_daemon(fips_control_socket, 5000) != 0) {
            if (signer) md_signer_destroy(signer);
            return 1;
        }
    }

    /* Initialize session state */
    MdSession session;
    md_session_init(&session);

    /* Session log for agent monitoring (M2.4) */
    MdSessionLogConfig slog_cfg = {
        .signer  = signer,
        .publish = (signer != NULL),  /* publish if signer available */
    };
    MdSessionLog *session_log = md_session_log_create(&slog_cfg);
    if (session_log)
        printf("host: session log ready\n");

    /* Host context (needed before TCP for nostr callbacks) */
    HostCtx ctx = {
        .session     = &session,
        .session_log = session_log,
    };
    pthread_mutex_init(&ctx.client_mu, NULL);
    if (expected_client_pk[0])
        strncpy(ctx.expected_client_pk, expected_client_pk, sizeof(ctx.expected_client_pk) - 1);

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

            /* Wire nostr into session log for relay publishing */
            if (session_log)
                md_session_log_set_nostr(session_log, nostr);

            /* FIPS reachability is now owned by the local FIPS daemon.
             * Do not publish legacy kind:30078/d=fips-transport IPv6 adverts
             * as the primary bootstrap signal. */

            /* Subscribe to allowlist updates */
            md_nostr_refresh_allowlist(nostr);

            printf("host: nostr bridge ready, waiting for session requests...\n");

            /* Wait for a session request DM (event-driven with timeout) */
            ctx.session_req_ch = go_channel_create(1);
            {
                void *dummy = NULL;
                GoSelectCase cases[] = {
                    { .op = GO_SELECT_RECEIVE, .chan = ctx.session_req_ch, .recv_buf = &dummy },
                };
                go_select_timeout(cases, 1, 60000); /* 60s timeout */
            }

            if (ctx.session_requested) {
                if (fips_npub) {
                    printf("host: waiting for FIPS peer readiness for %.*s...\n",
                           12, fips_npub);
                    MdFipsPeerReadiness ready;
                    MdFipsPeerReadinessState rstate = md_fips_control_wait_peer_ready(
                        fips_control_socket, fips_npub, fips_ready_timeout_ms,
                        MD_FIPS_CONTROL_DEFAULT_PEER_POLL_MS, &ready);
                    if (rstate != MD_FIPS_PEER_READY) {
                        if (rstate == MD_FIPS_PEER_NOT_FOUND)
                            host_print_fips_setup_guidance(fips_npub, &ready);
                        else
                            fprintf(stderr, "ERROR: FIPS route not ready (%s): %s\n",
                                    md_fips_peer_readiness_string(rstate), ready.detail);
                        if (ctx.session_req_ch) {
                            go_channel_close(ctx.session_req_ch);
                            go_channel_unref(ctx.session_req_ch);
                            ctx.session_req_ch = NULL;
                        }
                        md_nostr_destroy(nostr);
                        if (session_log) md_session_log_destroy(session_log);
                        return 1;
                    }
                    printf("host: FIPS peer ready: %s\n", ready.detail);
                }

                /* Generate cryptographically random session ID */
                char session_id[64];
                {
                    uint8_t rnd[16];
    #if defined(__APPLE__) || defined(__FreeBSD__)
                    arc4random_buf(rnd, sizeof(rnd));
    #elif defined(__linux__)
                    /* getrandom(2) — available since Linux 3.17 */
                    extern long getrandom(void *buf, size_t buflen, unsigned int flags);
                    if (getrandom(rnd, sizeof(rnd), 0) != sizeof(rnd)) {
                        /* Fallback to /dev/urandom */
                        FILE *f = fopen("/dev/urandom", "rb");
                        if (f) { fread(rnd, 1, sizeof(rnd), f); fclose(f); }
                        else memset(rnd, 0, sizeof(rnd));
                    }
    #else
                    FILE *f = fopen("/dev/urandom", "rb");
                    if (f) { fread(rnd, 1, sizeof(rnd), f); fclose(f); }
                    else memset(rnd, 0, sizeof(rnd));
    #endif
                    snprintf(session_id, sizeof(session_id),
                             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                             rnd[0],rnd[1],rnd[2],rnd[3], rnd[4],rnd[5],
                             rnd[6],rnd[7], rnd[8],rnd[9],
                             rnd[10],rnd[11],rnd[12],rnd[13],rnd[14],rnd[15]);
                }

                /* Determine granted capabilities */
                uint32_t granted = ctx.pending_req.capabilities &
                                   (MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT);

                /* Send session accept DM — confirm the tree format we'll use */
                MdSessionAccept acc = {
                    .granted = granted,
                    .tree_format = ctx.pending_req.tree_format,
                };
                strncpy(acc.session_id, session_id, sizeof(acc.session_id) - 1);

                char *acc_json = md_session_accept_to_json(&acc);
                if (acc_json) {
                    md_nostr_send_session_accept(nostr, ctx.pending_client_pk,
                                                acc_json);
                    free(acc_json);
                    printf("host: sent session accept to %.*s... (session=%s)\n",
                           8, ctx.pending_client_pk, session_id);

                    /* Log session accept */
                    if (session_log)
                        md_session_log_event(session_log, MD_SESSION_LOG_ACCEPT,
                                             session_id, ctx.pending_client_pk,
                                             "session accepted");
                }

                /* Update session state */
                md_session_request(&session, ctx.pending_client_pk,
                                   ctx.pending_req.capabilities,
                                   ctx.pending_req.tree_format);
                md_session_accept(&session, session_id, granted);
            } else if (g_running) {
                if (fips_npub) {
                    fprintf(stderr, "ERROR: timed out waiting for authorized --npub session request; refusing direct TCP fallback\n");
                    if (ctx.session_req_ch) {
                        go_channel_close(ctx.session_req_ch);
                        go_channel_unref(ctx.session_req_ch);
                        ctx.session_req_ch = NULL;
                    }
                    md_nostr_destroy(nostr);
                    if (session_log) md_session_log_destroy(session_log);
                    return 1;
                }
                printf("host: no session request received, accepting direct TCP\n");
            }
        } else {
            if (fips_npub) {
                fprintf(stderr, "ERROR: nostr bridge creation failed; --npub cannot fall back to direct TCP\n");
                if (session_log) md_session_log_destroy(session_log);
                if (signer) md_signer_destroy(signer);
                return 1;
            }
            fprintf(stderr, "WARNING: nostr bridge creation failed, direct TCP only\n");
        }
    }

#ifdef MD_ENABLE_FIPSNAT
    /* ── legacy fips-nat IPC connection (deprecated) ───────────── */
    MdIpcConn *fipsnat_conn = NULL;
    if (use_fipsnat) {
        printf("host: connecting to fips-nat daemon (%s)...\n", fipsnat_ipc_name);
        fipsnat_conn = md_ipc_connect(fipsnat_ipc_name, 5000);
        if (fipsnat_conn) {
            /* Query daemon status */
            char *cmd = md_fipsnat_ipc_cmd_status();
            if (cmd) {
                if (md_ipc_send(fipsnat_conn, cmd, strlen(cmd)) < 0) {
                    fprintf(stderr, "host: WARNING: failed to send fips-nat status request\n");
                }
                free(cmd);

                char resp_buf[MD_IPC_MAX_MSG];
                int n = md_ipc_recv(fipsnat_conn, resp_buf, sizeof(resp_buf) - 1, 5000);
                if (n > 0) {
                    resp_buf[n] = '\0';
                    MdFipsnatStatusResp st;
                    if (md_fipsnat_ipc_parse_status_response(resp_buf, &st) == 0) {
                        printf("host: fips-nat status: stun=%s published=%s",
                               st.stun_ok ? "ok" : "fail",
                               st.published ? "yes" : "no");
                        if (st.stun_ok)
                            printf(" addr=%s:%u", st.ip, st.port);
                        printf("\n");
                    }
                }
            }
        } else {
            fprintf(stderr, "host: WARNING: could not connect to legacy fips-nat daemon\n");
            fprintf(stderr, "  fips-nat is deprecated; prefer the FIPS daemon control path for discovery/readiness.\n");
            fprintf(stderr, "  Only start fips-nat manually when testing the legacy NAT IPC path.\n");
        }
    }
#endif

    /* ── MCP agent interface ──────────────────────────────────── */
    if (mcp_stdio || mcp_http) {
        const char *mode = mcp_stdio ? "stdio" : "http";
        printf("host: starting MCP server on %s\n", mode);

        /* Initialize a11y + input for MCP mode */
        MdA11yCtx *mcp_a11y = md_a11y_create();
        if (mcp_a11y)
            printf("host[mcp]: accessibility tree connected\n");
        else
            fprintf(stderr, "host[mcp]: WARNING: a11y unavailable\n");

        MdInputConfig mcp_input_cfg = {
            .screen_width = 1920, .screen_height = 1080,
        };
        MdInput *mcp_input = md_input_create(&mcp_input_cfg);
        if (mcp_input && md_input_is_ready(mcp_input))
            printf("host[mcp]: input injection ready\n");
        else
            fprintf(stderr, "host[mcp]: WARNING: input unavailable\n");

        MdMcpBridgeConfig mcp_cfg = {
            .a11y = mcp_a11y,
            .input = mcp_input,
            .tree_format = MD_TREE_FORMAT_JSON,
            .settle_ms = 100,
            .stdio_in_fd = mcp_stdio ? STDIN_FILENO : -1,
            .stdio_out_fd = mcp_stdio ? STDOUT_FILENO : -1,
        };

        MdMcpBridge *mcp_bridge = md_mcp_bridge_create(&mcp_cfg);
        if (!mcp_bridge) {
            fprintf(stderr, "ERROR: failed to create MCP bridge\n");
            return 1;
        }

        int mcp_rc = 0;

        if (mcp_http) {
            /* HTTP+SSE transport */
            MdMcpHttpConfig http_cfg = {
                .server = md_mcp_bridge_get_server(mcp_bridge),
                .port = mcp_http_port,
            };
            MdMcpHttp *http = md_mcp_http_create(&http_cfg);
            if (!http) {
                fprintf(stderr, "ERROR: failed to create MCP HTTP server\n");
                md_mcp_bridge_destroy(mcp_bridge);
                return 1;
            }
            uint16_t actual_port = mcp_http_port > 0
                                   ? mcp_http_port
                                   : MD_MCP_HTTP_DEFAULT_PORT;
            printf("host[mcp]: HTTP+SSE listening on port %u\n", actual_port);
            mcp_rc = md_mcp_http_run(http);
            md_mcp_http_destroy(http);
        } else {
            /* stdio transport (blocking — exits on EOF or shutdown) */
            mcp_rc = md_mcp_bridge_run(mcp_bridge);
        }

        md_mcp_bridge_destroy(mcp_bridge);

        if (signer) md_signer_destroy(signer);
        if (mcp_a11y) md_a11y_destroy(mcp_a11y);
        if (mcp_input) md_input_destroy(mcp_input);
        printf("host: MCP session ended (rc=%d)\n", mcp_rc);
        return mcp_rc < 0 ? 1 : 0;
    }

    /* Create TCP server */
    MdStreamServer *srv = md_stream_server_create(bind_addr, port);
    if (!srv) {
        fprintf(stderr, "ERROR: failed to bind TCP server on port %u\n", port);
        return 1;
    }
    printf("host: listening on port %u\n", port);

    /* Initialize encoder (persistent across reconnections) */
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
        md_stream_server_destroy(srv);
        return 1;
    }
    printf("host: encoder ready (%s)\n",
           md_encoder_is_hw(encoder) ? "NVENC" : "x264");

    /* Initialize input injection (persistent) */
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

    /* Initialize accessibility tree walker (persistent) */
    MdA11yCtx *a11y = md_a11y_create();
    if (!a11y) {
        fprintf(stderr, "WARNING: accessibility tree unavailable — agent mode disabled\n");
    } else {
        printf("host: accessibility tree connected\n");
    }

    /* Initialize agent action handler (persistent) */
    MdAgentConfig agent_cfg = {
        .a11y        = a11y,
        .input       = input,
        .tree_format = session.tree_format,
        .settle_ms   = MD_AGENT_DEFAULT_SETTLE_MS,
    };
    MdAgent *agent = md_agent_create(&agent_cfg);

    /* Set persistent host context fields */
    ctx.encoder = encoder;
    ctx.agent   = agent;

    /* Start screen capture (persistent — runs independently of clients) */
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
                cap_thr_ctx.host    = &ctx;
                cap_thr_ctx.capture = capture;
                if (pthread_create(&cap_thread, NULL, capture_thread_func, &cap_thr_ctx) == 0)
                    cap_thread_started = true;
            }
        }
    }

    /* ══════════════════════════════════════════════════════════════
     * Client accept + stream loop
     *
     * The host accepts a client, streams until disconnect, then
     * loops back to accept the next connection.  Persistent resources
     * (encoder, capture, a11y, agent) stay alive across reconnections.
     * ══════════════════════════════════════════════════════════════ */
    uint32_t client_num = 0;

    while (g_running) {
        /* Wait for client connection */
        printf("host: waiting for client...\n");
        MdStream *client = NULL;
        while (g_running && !client) {
            client = md_stream_server_accept(srv, 1000);
        }
        if (!client || !g_running) break;

        client_num++;
        printf("host: client #%u connected\n", client_num);

        /* Log connection */
        if (session_log)
            md_session_log_event(session_log, MD_SESSION_LOG_CONNECT,
                                 session.session_id,
                                 session.peer_npub[0] ? session.peer_npub : NULL,
                                 "TCP client connected");

        /* Reset per-connection state */
        pthread_mutex_lock(&ctx.client_mu);
        ctx.client     = client;
        pthread_mutex_unlock(&ctx.client_mu);
        ctx.pkt_seq    = 0;
        ctx.frames_sent = 0;
        ctx.total_encode_us = 0;
        ctx.frames_encoded  = 0;

        /* Send initial UI tree to new client */
        if (agent && a11y) {
            md_agent_send_tree(agent, client, &ctx.pkt_seq);
            printf("host: sent initial UI tree to client #%u\n", client_num);
        }

        /* ── Streaming loop ─────────────────────────────────────── */
        printf("host: streaming to client #%u (Ctrl+C to stop)\n", client_num);

        uint32_t last_stats_ms = md_stream_now_ms();
        uint32_t last_ping_ms = last_stats_ms;

        /* Adaptive bitrate controller for this connection */
        MdBitrateCtrlConfig br_cfg = {
            .max_bitrate = bitrate,
            .min_bitrate = bitrate / 16,  /* 1/16 of max as floor */
            .initial_bitrate = bitrate,
        };
        MdBitrateCtrl *br_ctrl = md_bitrate_ctrl_create(&br_cfg);
        uint32_t last_br = bitrate;

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

                /* Feed RTT to bitrate controller */
                if (br_ctrl && encoder) {
                    MdStreamStats st;
                    md_stream_get_stats(client, &st);
                    if (st.avg_rtt_ms > 0) {
                        uint32_t new_br = md_bitrate_ctrl_update(
                            br_ctrl, st.avg_rtt_ms, now);
                        if (new_br != last_br) {
                            md_encoder_set_bitrate(encoder, new_br);
                            last_br = new_br;
                            printf("host: bitrate adjusted to %u bps "
                                   "(rtt_avg=%ums, action=%s)\n",
                                   new_br, st.avg_rtt_ms,
                                   md_bitrate_ctrl_get_action(br_ctrl)
                                       == MD_BITRATE_DECREASE ? "decrease"
                                   : md_bitrate_ctrl_get_action(br_ctrl)
                                       == MD_BITRATE_INCREASE ? "increase"
                                   : "hold");
                        }
                    }
                }
            }

            /* Print stats every 5 seconds */
            if (now - last_stats_ms >= 5000) {
                MdStreamStats stats;
                md_stream_get_stats(client, &stats);

                double avg_encode_ms = ctx.frames_encoded > 0
                    ? (double)ctx.total_encode_us / ctx.frames_encoded / 1000.0
                    : 0.0;

                printf("host: [#%u] frames=%u sent=%u encode_avg=%.1fms "
                       "rtt=%ums rtt_avg=%ums tx=%.1fMB rx=%.1fKB\n",
                       client_num,
                       ctx.frames_encoded, ctx.frames_sent,
                       avg_encode_ms,
                       stats.last_rtt_ms, stats.avg_rtt_ms,
                       (double)stats.bytes_sent / (1024.0 * 1024.0),
                       (double)stats.bytes_recv / 1024.0);

                last_stats_ms = now;
            }
        }

        /* Client disconnected — clean up per-connection resources */
        md_bitrate_ctrl_destroy(br_ctrl);
        br_ctrl = NULL;

        /* Restore original bitrate for next connection */
        if (encoder)
            md_encoder_set_bitrate(encoder, bitrate);

        /* Log disconnection */
        if (session_log) {
            char disc_detail[128];
            snprintf(disc_detail, sizeof(disc_detail),
                     "disconnected after %u frames", ctx.frames_sent);
            md_session_log_event(session_log, MD_SESSION_LOG_DISCONNECT,
                                 session.session_id,
                                 session.peer_npub[0] ? session.peer_npub : NULL,
                                 disc_detail);
        }

        printf("host: client #%u disconnected (sent %u frames)\n",
               client_num, ctx.frames_sent);

        /* Flush any buffered encoder output before destroying the stream */
        md_encoder_flush(encoder, on_encoded, &ctx);

        /* Null out client before destroying so capture thread stops sending */
        pthread_mutex_lock(&ctx.client_mu);
        ctx.client = NULL;
        pthread_mutex_unlock(&ctx.client_mu);
        md_stream_destroy(client);

        if (!g_running) break;

        printf("host: ready for next connection\n");
    }

    /* ── Shutdown ────────────────────────────────────────────────── */
    printf("\nhost: shutting down...\n");

    if (capture) {
        md_capture_stop(capture);
        if (cap_thread_started)
            pthread_join(cap_thread, NULL);
        md_capture_destroy(capture);
    }

    md_encoder_destroy(encoder);

    if (agent) {
        printf("host: handled %u agent actions\n", md_agent_get_action_count(agent));
        md_agent_destroy(agent);
    }
    if (a11y)
        md_a11y_destroy(a11y);
    if (input)
        md_input_destroy(input);

    md_stream_server_destroy(srv);

    if (session_log)
        md_session_log_destroy(session_log);

#ifdef MD_ENABLE_FIPSNAT
    if (fipsnat_conn)
        md_ipc_close(fipsnat_conn);
#endif

    if (nostr) {
        go_channel_close(ctx.session_req_ch);
        go_channel_unref(ctx.session_req_ch);
        md_nostr_destroy(nostr);
    } else if (signer)
        md_signer_destroy(signer);

    pthread_mutex_destroy(&ctx.client_mu);

    printf("host: done. served %u client(s)\n", client_num);
    return 0;
}
