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
#include "fips_control.h"
#include "session.h"
#include "packet.h"
#include "stream.h"
#include "decode.h"
#include "render.h"
#include "signer.h"
#include "nostr.h"
#include "a11y.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <go.h>

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

    /* UI tree state (received from host agent) */
    char         *ui_tree_json;         /* latest full UI tree JSON (owned)   */
    size_t        ui_tree_len;          /* byte length of ui_tree_json        */
    uint32_t      tree_updates;         /* count of tree/delta packets recv'd */

    /* Nostr session negotiation state */
    char          expected_host_pk[128]; /* pubkey hex of host we're connecting to */
    char          accepted_session_id[64]; /* from session_accept DM     */
    uint32_t      granted_caps;       /* from session_accept DM          */
    MdTreeFormat  accepted_tree_format; /* confirmed tree format          */
    volatile int  session_accepted;   /* set by on_dm callback           */
    GoChannel      *session_ch;       /* signaled on session accepted    */

    /* Connection recovery UI state */
    int            reconnecting;      /* unexpected disconnect recovery  */
    uint32_t       reconnect_delay_ms; /* next retry delay for overlay     */
    const char    *connection_status; /* transient user-facing detail     */
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

    /* Overlay stats/rendering are driven from the main loop. */
}

/* ── Nostr callbacks for --npub session negotiation ──────────── */

static void on_session_dm(const char *sender_pubkey_hex, const char *content,
                          void *userdata) {
    ClientCtx *ctx = userdata;
    if (!ctx || !content || !sender_pubkey_hex) return;

    /* Verify the DM came from the expected host */
    if (ctx->expected_host_pk[0] != '\0' &&
        strcmp(sender_pubkey_hex, ctx->expected_host_pk) != 0) {
        fprintf(stderr, "client: ignoring session DM from unexpected pubkey %.*s...\n",
                8, sender_pubkey_hex);
        return;
    }

    /* Try to parse as session_accept */
    MdSessionAccept acc;
    if (md_session_accept_from_json(content, &acc) == 0) {
        strncpy(ctx->accepted_session_id, acc.session_id,
                sizeof(ctx->accepted_session_id) - 1);
        ctx->granted_caps = acc.granted;
        ctx->accepted_tree_format = acc.tree_format;
        ctx->session_accepted = 1;
        if (ctx->session_ch)
            go_channel_try_send(ctx->session_ch, (void *)(uintptr_t)1);
    }
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
    fprintf(stderr, "  --timeout MS       Connect/readiness timeout (default: 5000)\n");
    fprintf(stderr, "  --fips-control PATH  FIPS daemon control socket override\n");
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


static void print_fips_setup_guidance(const char *prefix, const char *npub,
                                      const MdFipsPeerReadiness *ready) {
    fprintf(stderr,
            "ERROR: %s: FIPS peer not configured or not discovered by local daemon (%.*s...)\n",
            prefix, 12, npub ? npub : "");
    if (ready && ready->detail[0])
        fprintf(stderr, "  FIPS detail: %s\n", ready->detail);
    fprintf(stderr,
            "  Configure this peer in the FIPS daemon (via_nostr/auto_connect/peer discovery)\n"
            "  and verify `fipsctl show peers` reports a connected peer before using metadesk --npub.\n");
}

static int check_fips_daemon(const char *socket_path, uint32_t timeout_ms) {
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
    printf("client: FIPS daemon ready");
    if (resp.socket_path) printf(" (%s)", resp.socket_path);
    if (cJSON_IsString(state) && state->valuestring) printf(" state=%s", state->valuestring);
    if (cJSON_IsString(npub) && npub->valuestring) printf(" npub=%.*s...", 12, npub->valuestring);
    printf("\n");
    md_fips_control_response_free(&resp);
    return 0;
}

static void cleanup_npub_nostr(MdNostr **nostr, ClientCtx *ctx) {
    if (ctx && ctx->session_ch) {
        go_channel_close(ctx->session_ch);
        go_channel_unref(ctx->session_ch);
        ctx->session_ch = NULL;
    }
    if (nostr && *nostr) {
        md_nostr_destroy(*nostr);
        *nostr = NULL;
    }
}

static void reset_session_accept_state(ClientCtx *ctx) {
    if (!ctx) return;
    ctx->accepted_session_id[0] = '\0';
    ctx->granted_caps = 0;
    ctx->accepted_tree_format = MD_TREE_FORMAT_COMPACT;
    ctx->session_accepted = 0;
}

static int negotiate_npub_session(MdNostr *nostr, ClientCtx *ctx,
                                  const char *host_pubkey_hex,
                                  uint32_t timeout_ms,
                                  MdSessionRequest *out_req) {
    if (!nostr || !ctx || !host_pubkey_hex || !out_req)
        return -1;

    reset_session_accept_state(ctx);

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
        return -1;
    }

    printf("client: sending session request DM...\n");
    if (md_nostr_send_session_request(nostr, host_pubkey_hex, req_json) != 0) {
        fprintf(stderr, "ERROR: failed to send session request\n");
        free(req_json);
        return -1;
    }
    free(req_json);

    void *dummy = NULL;
    GoSelectCase cases[] = {
        { .op = GO_SELECT_RECEIVE, .chan = ctx->session_ch, .recv_buf = &dummy },
    };
    go_select_timeout(cases, 1, (uint64_t)timeout_ms * 2);

    if (!ctx->session_accepted) {
        fprintf(stderr, "ERROR: timed out waiting for session accept\n");
        return -1;
    }

    printf("client: session accepted (id=%s)\n", ctx->accepted_session_id);
    *out_req = req;
    return 0;
}

static int send_session_info(MdStream *stream, const ClientCtx *ctx) {
    if (!stream || !ctx)
        return -1;

    MdSessionAccept acc = { .tree_format = ctx->accepted_tree_format };
    strncpy(acc.session_id, ctx->accepted_session_id, sizeof(acc.session_id) - 1);
    acc.granted = ctx->granted_caps;

    char *info_json = md_session_accept_to_json(&acc);
    if (!info_json)
        return -1;

    int ret = md_stream_send(stream, MD_PKT_SESSION_INFO, 0,
                             (const uint8_t *)info_json,
                             (uint32_t)strlen(info_json));
    free(info_json);
    return ret;
}

static MdStream *connect_client_stream(const char *host, const char *npub,
                                       const char *fips_control_socket,
                                       uint16_t port, uint32_t timeout_ms,
                                       MdNostr *nostr,
                                       const char *host_pubkey_hex,
                                       ClientCtx *ctx,
                                       MdSession *session) {
    MdStream *stream = NULL;

    if (npub) {
        printf("client: checking FIPS daemon health...\n");
        if (check_fips_daemon(fips_control_socket, timeout_ms) != 0)
            return NULL;

        printf("client: waiting for FIPS peer readiness for %.*s...\n", 12, npub);
        MdFipsPeerReadiness ready;
        MdFipsPeerReadinessState rstate = md_fips_control_wait_peer_ready(
            fips_control_socket, npub, timeout_ms,
            MD_FIPS_CONTROL_DEFAULT_PEER_POLL_MS, &ready);
        if (rstate != MD_FIPS_PEER_READY) {
            if (rstate == MD_FIPS_PEER_NOT_FOUND)
                print_fips_setup_guidance("client", npub, &ready);
            else
                fprintf(stderr, "ERROR: FIPS route not ready (%s): %s\n",
                        md_fips_peer_readiness_string(rstate), ready.detail);
            return NULL;
        }
        printf("client: FIPS peer ready: %s\n", ready.detail);

        MdSessionRequest req;
        if (negotiate_npub_session(nostr, ctx, host_pubkey_hex,
                                   timeout_ms, &req) != 0)
            return NULL;

        printf("client: connecting via FIPS to %.*s...\n", 12, npub);
        stream = md_stream_connect_fips(npub, port, timeout_ms);
        if (!stream) {
            fprintf(stderr, "ERROR: FIPS connect failed\n");
            return NULL;
        }

        if (send_session_info(stream, ctx) != 0) {
            fprintf(stderr, "ERROR: failed to send session info\n");
            md_stream_destroy(stream);
            return NULL;
        }

        md_session_reset(session);
        md_session_request(session, npub, req.capabilities,
                           ctx->accepted_tree_format);
        md_session_accept(session, ctx->accepted_session_id,
                          ctx->granted_caps);
        md_session_activate(session);
    } else {
        printf("client: connecting to %s:%u...\n", host, port);
        stream = md_stream_connect(host, port, timeout_ms);
        if (!stream)
            fprintf(stderr, "ERROR: failed to connect to %s:%u\n", host, port);
    }

    return stream;
}

static void render_client_overlay(ClientCtx *ctx, MdStream *stream,
                                  uint32_t stats_start_ms) {
    if (!ctx || !ctx->overlay)
        return;

    MdStreamStats stream_stats = { 0 };
    if (stream)
        md_stream_get_stats(stream, &stream_stats);

    uint32_t now = md_stream_now_ms();
    double avg_decode_ms = ctx->frames_decoded > 0
        ? (double)ctx->total_decode_us / ctx->frames_decoded / 1000.0
        : 0.0;

    MdOverlayStats overlay_stats = {
        .latency_ms   = (float)(avg_decode_ms + stream_stats.avg_rtt_ms),
        .encode_ms    = 0.0f,
        .decode_ms    = (float)avg_decode_ms,
        .rtt_ms       = (float)stream_stats.avg_rtt_ms,
        .connected    = stream && md_stream_is_connected(stream),
        .reconnecting = ctx->reconnecting != 0,
        .reconnect_delay_ms = ctx->reconnect_delay_ms,
        .status_message = ctx->connection_status,
        .fps          = ctx->frames_decoded > 0
            ? (int)(ctx->frames_decoded * 1000.0 /
                    (now - stats_start_ms + 1)) : 0,
        .bitrate_mbps = stream_stats.bytes_recv > 0
            ? (float)(stream_stats.bytes_recv * 8.0 / 1000000.0) : 0.0f,
        .encoder_name = NULL,
    };

    md_overlay_new_frame(ctx->overlay);
    md_overlay_render(ctx->overlay, &overlay_stats);
}

static int sleep_with_reconnect_ui(ClientCtx *ctx, uint32_t delay_ms,
                                   bool *deliberate_exit) {
    uint32_t start = md_stream_now_ms();

    while (g_running) {
        uint32_t now = md_stream_now_ms();
        uint32_t elapsed = now - start;
        if (elapsed >= delay_ms)
            return 0;

        if (ctx) {
            ctx->reconnect_delay_ms = delay_ms - elapsed;
            if (ctx->renderer && md_renderer_poll_events(ctx->renderer) < 0) {
                if (deliberate_exit)
                    *deliberate_exit = true;
                g_running = 0;
                return -1;
            }
            render_client_overlay(ctx, NULL, start);
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
            if (!g_running)
                return -1;
        }
    }

    return -1;
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
    const char *fips_control_socket = NULL;
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
        else if (strcmp(argv[i], "--fips-control") == 0 && i + 1 < argc)
            fips_control_socket = argv[++i];
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
    char host_pubkey_hex[65] = { 0 };

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

        /* Session DM callback. FIPS reachability is owned by the FIPS daemon
         * and checked through fips_control; do not subscribe to legacy
         * kind:30078 transport adverts here. */
        MdNostrCallbacks nostr_cbs = { 0 };
        nostr_cbs.on_dm = on_session_dm;
        nostr_cbs.dm_userdata = &ctx;

        MdNostrConfig nostr_cfg = {
            .signer = signer,
            .relay_urls = relay_urls,
            .relay_count = relay_count,
        };

        /* Create channel before nostr_create — callbacks fire on worker threads */
        ctx.session_ch = go_channel_create(1);

        nostr = md_nostr_create(&nostr_cfg, &nostr_cbs);
        if (!nostr) {
            fprintf(stderr, "ERROR: failed to create Nostr bridge\n");
            go_channel_close(ctx.session_ch);
            go_channel_unref(ctx.session_ch);
            ctx.session_ch = NULL;
            if (signer) md_signer_destroy(signer);
            return 1;
        }

        if (md_fips_npub_to_pubkey_hex(npub, host_pubkey_hex, sizeof(host_pubkey_hex)) != 0) {
            fprintf(stderr, "ERROR: --npub must be a valid bech32 FIPS/Nostr npub identity\n");
            cleanup_npub_nostr(&nostr, &ctx);
            return 1;
        }

        /* Store expected host pubkey hex for callback verification */
        strncpy(ctx.expected_host_pk, host_pubkey_hex, sizeof(ctx.expected_host_pk) - 1);
        ctx.expected_host_pk[sizeof(ctx.expected_host_pk) - 1] = '\0';
    }

    stream = connect_client_stream(host, npub, fips_control_socket,
                                   port, timeout_ms, nostr,
                                   host_pubkey_hex, &ctx, &session);
    if (!stream) {
        cleanup_npub_nostr(&nostr, &ctx);
        if (signer) md_signer_destroy(signer);
        return 1;
    }
    printf("client: connected\n");

    /* Create decoder */
    MdDecoder *decoder = md_decoder_create();
    if (!decoder) {
        fprintf(stderr, "ERROR: failed to create H.264 decoder\n");
        md_stream_destroy(stream);
        cleanup_npub_nostr(&nostr, &ctx);
        if (signer) md_signer_destroy(signer);
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
    uint32_t reconnect_delay_ms = 1000;
    const uint32_t reconnect_delay_max_ms = 30000;
    bool deliberate_exit = false;

    while (g_running) {
        if (!stream || !md_stream_is_connected(stream)) {
            if (stream) {
                md_stream_destroy(stream);
                stream = NULL;
            }
            md_session_reset(&session);

            ctx.reconnecting = 1;
            ctx.connection_status = "Disconnected — reconnecting...";
            ctx.reconnect_delay_ms = reconnect_delay_ms;
            render_client_overlay(&ctx, NULL, last_stats_ms);

            fprintf(stderr, "client: reconnecting in %.1fs\n",
                    (double)reconnect_delay_ms / 1000.0);
            if (sleep_with_reconnect_ui(&ctx, reconnect_delay_ms,
                                        &deliberate_exit) != 0 ||
                deliberate_exit || !g_running) {
                break;
            }

            stream = connect_client_stream(host, npub, fips_control_socket,
                                           port, timeout_ms, nostr,
                                           host_pubkey_hex, &ctx, &session);
            if (stream) {
                ctx.reconnecting = 0;
                ctx.reconnect_delay_ms = 0;
                ctx.connection_status = NULL;
                reconnect_delay_ms = 1000;
                last_stats_ms = md_stream_now_ms();
                printf("client: reconnected\n");
                continue;
            }

            if (reconnect_delay_ms < reconnect_delay_max_ms) {
                reconnect_delay_ms *= 2;
                if (reconnect_delay_ms > reconnect_delay_max_ms)
                    reconnect_delay_ms = reconnect_delay_max_ms;
            }
            continue;
        }

        /* Check SDL events (window close, Escape, etc.) */
        if (ctx.renderer) {
            if (md_renderer_poll_events(ctx.renderer) < 0) {
                printf("client: window closed\n");
                deliberate_exit = true;
                g_running = 0;
                break;
            }
        }

        /* Receive next packet */
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        int ret = md_stream_recv(stream, &hdr, &payload, 16 /* ~1 frame at 60fps */);

        if (ret == 1) {
            /* Timeout — no data yet, keep UI responsive and loop back. */
            render_client_overlay(&ctx, stream, last_stats_ms);
            continue;
        }
        if (ret < 0) {
            /* Unexpected connection loss — enter reconnect loop. */
            fprintf(stderr, "client: connection lost\n");
            md_stream_destroy(stream);
            stream = NULL;
            md_session_reset(&session);
            ctx.reconnecting = 1;
            ctx.connection_status = "Disconnected — reconnecting...";
            ctx.reconnect_delay_ms = reconnect_delay_ms;
            continue;
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
            /* Full UI tree snapshot from host agent.
             * Store the JSON for agent interaction / overlay display. */
            if (payload && hdr.payload_len > 0) {
                free(ctx.ui_tree_json);
                ctx.ui_tree_json = malloc(hdr.payload_len + 1);
                if (ctx.ui_tree_json) {
                    memcpy(ctx.ui_tree_json, payload, hdr.payload_len);
                    ctx.ui_tree_json[hdr.payload_len] = '\0';
                    ctx.ui_tree_len = hdr.payload_len;
                }
                ctx.tree_updates++;
                if (ctx.tree_updates <= 3 || (ctx.tree_updates % 100) == 0) {
                    fprintf(stderr, "client: received UI tree (%u bytes, update #%u)\n",
                            hdr.payload_len, ctx.tree_updates);
                }
            }
            break;

        case MD_PKT_UI_TREE_DELTA:
            /* Incremental UI tree delta from host agent.
             * Parse delta JSON and patch ui_tree_json in-place. */
            if (payload && hdr.payload_len > 0 && ctx.ui_tree_json) {
                char *delta_str = malloc(hdr.payload_len + 1);
                if (delta_str) {
                    memcpy(delta_str, payload, hdr.payload_len);
                    delta_str[hdr.payload_len] = '\0';

                    char *patched = md_a11y_tree_patch(ctx.ui_tree_json, delta_str);
                    free(delta_str);

                    if (patched) {
                        free(ctx.ui_tree_json);
                        ctx.ui_tree_json = patched;
                        ctx.ui_tree_len = strlen(patched);
                    }
                }
            }
            ctx.tree_updates++;
            if (ctx.tree_updates <= 3 || (ctx.tree_updates % 100) == 0) {
                fprintf(stderr, "client: applied UI tree delta (%u bytes, update #%u)\n",
                        hdr.payload_len, ctx.tree_updates);
            }
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
        render_client_overlay(&ctx, stream, last_stats_ms);

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

    cleanup_npub_nostr(&nostr, &ctx);
    if (signer)
        md_signer_destroy(signer);
    free(ctx.ui_tree_json);
    if (ctx.overlay)
        md_overlay_destroy(ctx.overlay);
    if (ctx.renderer)
        md_renderer_destroy(ctx.renderer);

    printf("client: done. decoded %u frames, displayed %u, tree updates %u\n",
           ctx.frames_decoded, ctx.frames_displayed, ctx.tree_updates);
    return 0;
}
