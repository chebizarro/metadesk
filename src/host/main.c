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
#include "input_event.h"
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
#include "bitrate_ctrl.h"
#include "session_log.h"
#include "host_mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <go.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

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
    /* A client vanishing mid-send must be an EPIPE on that stream, not a
     * fatal signal for the whole host (OpenSSL writes bypass MSG_NOSIGNAL). */
    signal(SIGPIPE, SIG_IGN);
}

/* ── State shared between capture callback and main loop ─────── */

typedef struct {
    MdEncoder    *encoder;
    MdStream     *client;
    pthread_mutex_t client_mu;  /* protects client pointer lifetime */
    MdAgent      *agent;
    MdInput      *input;        /* raw human input injection (MD_PKT_INPUT) */
    MdNostr      *nostr;
    MdSession    *session;
    uint32_t      frame_seq;
    uint32_t      pkt_seq;
    int64_t       total_encode_us;
    uint32_t      frames_encoded;
    uint32_t      frames_sent;
    bool          frame_size_mismatch_warned;

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
    if (!md_stream_has_capability(client, MD_CAP_VIDEO)) {
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

static void host_owner_revoked(void *data) {
    HostCtx *ctx = data;
    g_running = 0;
    if (ctx->session_req_ch) go_channel_close(ctx->session_req_ch);
}


static void host_on_dm(const char *sender_pubkey_hex, const char *content,
                       const MdNostrDmInfo *info, void *userdata) {
    HostCtx *ctx = userdata;
    if (!ctx || !content || !info || ctx->session_requested) return;

    /* Try to parse as session_request */
    MdSessionRequest req;
    if (md_session_request_from_json(content, &req) == 0) {
        if (ctx->expected_client_pk[0] != '\0' &&
            strcmp(sender_pubkey_hex, ctx->expected_client_pk) != 0) {
            fprintf(stderr, "host: REJECTED session request from unexpected FIPS client %.*s...\n",
                    8, sender_pubkey_hex);
            return;
        }

        /* md-6ns: a request stored before this host subscribed was addressed
         * to an earlier host instance (NIP-59 timestamps defeat `since`).
         * Ignore history and stale/uncorrelated requests without latching,
         * so the client's live request can still be answered. */
        if (!info->live) {
            fprintf(stderr, "host: ignoring session request from relay history "
                    "(%.*s..., request_id=%.8s)\n", 8, sender_pubkey_hex,
                    req.request_id[0] ? req.request_id : "none");
            return;
        }
        MdSessionRequestFreshness fresh = md_session_request_freshness(
            &req, (int64_t)time(NULL), info->rumor_created_at);
        if (fresh != MD_SESSION_REQUEST_FRESH) {
            fprintf(stderr, "host: ignoring %s session request from %.*s...\n",
                    md_session_request_freshness_name(fresh), 8, sender_pubkey_hex);
            return;
        }

        /* Enforce allowlist: if an allowlist is configured, reject
         * clients that are not on it. If no allowlist is configured
         * (open mode), accept all clients. */
        if (!ctx->nostr || !md_nostr_is_allowed(ctx->nostr, sender_pubkey_hex)) {
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

/* Exhaustive: an RGB-ordered frame labelled BGRX would swap red and blue. */
static MdPixFmt capture_fmt_to_enc(MdCapturePixFmt cfmt) {
    switch (cfmt) {
    case MD_PIX_CAPTURE_BGRX: return MD_PIX_FMT_BGRX;
    case MD_PIX_CAPTURE_BGRA: return MD_PIX_FMT_BGRA;
    case MD_PIX_CAPTURE_RGBX: return MD_PIX_FMT_RGBX;
    case MD_PIX_CAPTURE_RGBA: return MD_PIX_FMT_RGBA;
    case MD_PIX_CAPTURE_NV12: return MD_PIX_FMT_NV12;
    }
    return MD_PIX_FMT_BGRX; /* unreachable: backends emit only the above */
}

#define HOST_CAPTURE_SIZE_WAIT_MS 2000
/* Upper bound on one idle get_frame wait, so has_client and g_running are
 * re-sampled while a damage-driven stream is quiet. */
#define HOST_CAPTURE_FRAME_WAIT_MS 100
/* Pacing after MD_CAPTURE_ERROR (returned without waiting) while the
 * watchdog decides whether to restart. */
#define HOST_CAPTURE_ERROR_BACKOFF_US 10000
/* Restart only broken capture, not healthy damage-driven idle streams.
 * Authorization removal is separately opt-in and forbidden unattended. */
#define HOST_CAPTURE_STALL_MS      4000
#define HOST_CAPTURE_MAX_RESTARTS  2

static bool host_dimensions_are_valid(uint32_t width, uint32_t height) {
    return width >= MD_INPUT_MIN_SCREEN_DIMENSION &&
           height >= MD_INPUT_MIN_SCREEN_DIMENSION;
}

static void host_apply_fallback_dimensions(uint32_t *width, uint32_t *height,
                                           const char *reason) {
    *width = MD_INPUT_FALLBACK_SCREEN_WIDTH;
    *height = MD_INPUT_FALLBACK_SCREEN_HEIGHT;
    fprintf(stderr,
            "host: WARNING: using fallback display dimensions %ux%u (%s)\n",
            *width, *height,
            reason ? reason : "actual capture/display size unavailable");
}

static int host_wait_capture_dimensions(MdCaptureCtx *capture,
                                        uint32_t *width, uint32_t *height) {
    if (!capture || !width || !height)
        return -1;

    uint32_t w = 0, h = 0;
    if (md_capture_wait_size(capture, HOST_CAPTURE_SIZE_WAIT_MS, &w, &h) != 0)
        return -1;
    if (!host_dimensions_are_valid(w, h)) {
        fprintf(stderr,
                "host: WARNING: capture reported invalid dimensions %ux%u; "
                "minimum is %ux%u\n",
                w, h,
                MD_INPUT_MIN_SCREEN_DIMENSION,
                MD_INPUT_MIN_SCREEN_DIMENSION);
        return -1;
    }
    *width = w;
    *height = h;
    return 0;
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

    /* A restored or damage-driven stream can be paused but healthy. Keep
     * pulling frames; only MD_CAPTURE_ERROR arms the recovery watchdog. */
    MdCaptureWatchdog watchdog = {0};

    while (g_running) {
        pthread_mutex_lock(&ctx->client_mu);
        bool has_client = (ctx->client != NULL);
        pthread_mutex_unlock(&ctx->client_mu);
        MdFrame frame;
        int capture_result = md_capture_get_frame(cap, &frame,
                                                  HOST_CAPTURE_FRAME_WAIT_MS);
        MdCaptureRecovery recovery = md_capture_watchdog_step(&watchdog,
            capture_result, has_client, md_stream_now_ms(),
            HOST_CAPTURE_STALL_MS, HOST_CAPTURE_MAX_RESTARTS);
        if (capture_result != 0) {
            if (recovery != MD_CAPTURE_RECOVERY_NONE) {
                md_capture_stop(cap);
                if (recovery == MD_CAPTURE_RECOVERY_EXHAUSTED) {
                    bool forget = md_capture_stall_may_forget(&cap->config);
                    if (forget) md_capture_forget_authorization(cap);
                    fprintf(stderr,
                        "host: capture unrecoverable after %u restarts; "
                        "screencast authorization %s — operator action required\n",
                        watchdog.restarts, forget ? "cleared" : "retained");
                } else {
                    fprintf(stderr,
                        "host: capture broken for %dms with a client "
                        "connected — restarting capture (attempt %u)\n",
                        HOST_CAPTURE_STALL_MS, watchdog.restarts);
                    if (md_capture_start(cap) != 0)
                        fprintf(stderr, "host: capture restart failed\n");
                }
            }
            if (capture_result != MD_CAPTURE_IDLE)
                usleep(HOST_CAPTURE_ERROR_BACKOFF_US);
            continue;
        }

        if (!ctx->encoder || !has_client) {
            md_capture_release_frame(cap, &frame);
            continue;
        }

        /* The encoder converts enc_w x enc_h pixels using the frame's
         * stride, so a frame at most one row/column larger (odd display
         * edge rounded down) is a safe crop; anything else is dropped. */
        uint32_t enc_w = 0, enc_h = 0;
        if (md_encoder_get_size(ctx->encoder, &enc_w, &enc_h) == 0 &&
            (frame.width < enc_w || frame.height < enc_h ||
             frame.width - enc_w > 1 || frame.height - enc_h > 1)) {
            if (!ctx->frame_size_mismatch_warned) {
                fprintf(stderr,
                        "host: WARNING: dropping capture frames sized %ux%u; "
                        "encoder is configured for %ux%u\n",
                        frame.width, frame.height, enc_w, enc_h);
                ctx->frame_size_mismatch_warned = true;
            }
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

/* Publish only after listener/backend startup, so no valid proof is stranded
 * in the listen backlog while the client's bounded ACK wait expires. */
static int host_publish_session_accept(HostCtx *ctx, MdAccountBinding *owner) {
    if (!g_running || (owner && !md_account_binding_is_current(owner))) return -1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    MdSessionAccept acc;
    if (!ctx->pending_req.request_id[0] ||
        md_session_prepare_binding(ctx->session, &acc,
        (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000) != 0)
        return -1;
    /* Correlate the answer with exactly the request it grants (md-6ns). */
    memcpy(acc.request_id, ctx->pending_req.request_id, sizeof(acc.request_id));
    char *json = md_session_accept_to_json(&acc);
    OPENSSL_cleanse(acc.stream_token, sizeof(acc.stream_token));
    if (!json) return -1;
    int ret = md_nostr_send_session_accept(ctx->nostr, ctx->pending_client_pk, json);
    OPENSSL_cleanse(json, strlen(json));
    free(json);
    if (ret || !g_running || (owner && !md_account_binding_is_current(owner)))
        return -1;
    if (ctx->session_log)
        md_session_log_event(ctx->session_log, MD_SESSION_LOG_ACCEPT,
                             ctx->session->session_id, ctx->pending_client_pk,
                             "session accepted");
    return 0;
}

/* Candidate ownership stays on the host thread. It is never published here;
 * capture sees it only after the bind has consumed the reservation and ACKed. */
static int host_bind_candidate(HostCtx *ctx, MdStream *candidate) {
    if (ctx->client) {
        md_stream_close(candidate);
        return -1;
    }
    return md_stream_accept_binding(candidate, ctx->session);
}

static void host_send_initial_tree(HostCtx *ctx) {
    if (ctx->agent && md_stream_has_capability(ctx->client, MD_CAP_AGENT))
        md_agent_send_tree(ctx->agent, ctx->client, &ctx->pkt_seq);
}

/* ── Handle incoming packets from client ─────────────────────── */

static void handle_client_packet(HostCtx *ctx, const MdPacketHeader *hdr,
                                 const uint8_t *payload) {
    /* Only the published, proven stream can dispatch. Rebinding is always a
     * protocol violation, even on the stream that consumed the token. */
    uint32_t needed = 0;
    if (hdr->type == MD_PKT_INPUT) needed = MD_CAP_INPUT;
    if (hdr->type == MD_PKT_ACTION) needed = MD_CAP_AGENT | MD_CAP_INPUT;
    if (hdr->type == MD_PKT_SESSION_INFO ||
        !md_stream_has_capability(ctx->client, needed)) {
        md_stream_close(ctx->client);
        return;
    }
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

    case MD_PKT_INPUT: {
        /* Raw human input from the video client. */
        if (!ctx->input || !payload || hdr->payload_len == 0)
            break;
        uint8_t kind = 0, button = 0;
        int16_t x = 0, y = 0;
        uint16_t tlen = 0;
        const char *text = NULL;
        if (md_input_event_unpack(payload, hdr->payload_len, &kind, &button,
                                  &x, &y, &text, &tlen) != 0)
            break;
        switch (kind) {
        case MD_INPUT_EV_MOVE:
            md_input_mouse_move(ctx->input, x, y);
            break;
        case MD_INPUT_EV_BUTTON: {
            /* Wire buttons are 1=left, 2=middle, 3=right; md_input_click()
             * uses 0=left, 1=right, 2=middle. */
            int mb = (button == 2) ? 2 : (button == 3) ? 1 : 0;
            md_input_click(ctx->input, x, y, mb);
            break;
        }
        case MD_INPUT_EV_WHEEL:
            md_input_scroll(ctx->input, x, y);
            break;
        case MD_INPUT_EV_KEY: {
            const char *keys[MD_INPUT_MAX_COMBO_KEYS];
            int kc = 0;
            if (button & 1) keys[kc++] = "ctrl";
            if (button & 2) keys[kc++] = "shift";
            if (button & 4) keys[kc++] = "alt";
            if (button & 8) keys[kc++] = "super";
            char namez[64];
            size_t nl = tlen < sizeof(namez) - 1 ? tlen : sizeof(namez) - 1;
            if (text) memcpy(namez, text, nl);
            namez[nl] = '\0';
            if (kc < MD_INPUT_MAX_COMBO_KEYS && nl > 0) {
                keys[kc++] = namez;
                md_input_key_combo(ctx->input, keys, kc);
            }
            break;
        }
        case MD_INPUT_EV_TEXT: {
            char txt[MD_INPUT_EVENT_MAX_TEXT + 1];
            size_t tl = tlen < sizeof(txt) - 1 ? tlen : sizeof(txt) - 1;
            if (text) memcpy(txt, text, tl);
            txt[tl] = '\0';
            md_input_type_text(ctx->input, txt);
            break;
        }
        default:
            break;
        }
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
    fprintf(stderr, "  %s access --help  Manage saved host-role session access\n", argv0);
    fprintf(stderr, "  %s mcp-proxy --help  stdio MCP client -> NIP-98-signed HTTP MCP\n", argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --port PORT      Listen port (default: %d)\n", MD_STREAM_PORT);
    fprintf(stderr, "  --bind ADDR      Bind address (default: any)\n");
    fprintf(stderr, "  --fps FPS        Target framerate (default: 60)\n");
    fprintf(stderr, "  --bitrate BPS    Encoder bitrate (default: 8000000)\n");
    fprintf(stderr, "  --no-nvenc       Disable NVENC, use x264\n");
    fprintf(stderr, "  --no-capture     Skip screen capture (test mode)\n");
    fprintf(stderr, "  --capture-unattended  Portal consent budget <=10s; never auto-forget\n");
    fprintf(stderr, "  --capture-consent-timeout-ms MS  Total portal budget (1..180000)\n");
    fprintf(stderr, "  --capture-forget-on-stall  Opt in to token removal after broken capture\n");
    fprintf(stderr, "  --npub NPUB      Require FIPS client npub for auth\n");
    fprintf(stderr, "  --fips-control PATH  FIPS daemon control socket override\n");
    fprintf(stderr, "  --fips-ready-timeout MS  Peer readiness timeout (default: 10000)\n");
    fprintf(stderr, "\nSigner options (choose one):\n");
    fprintf(stderr, "  --bunker URI     NIP-46 Nostr Connect bunker URI\n");
    fprintf(stderr, "  --dbus-signer    Use NIP-55L D-Bus signer daemon\n");
    fprintf(stderr, "  --socket-signer [PATH]  Use NIP-5F Unix socket signer\n");
    fprintf(stderr, "  --auto-signer    Auto-detect local signer (NIP-5F, NIP-55L)\n");
    fprintf(stderr, "  --relay URL      Relay URL (default: " MD_DEFAULT_RELAY ")\n");
    fprintf(stderr, "\nMCP agent interface:\n");
    fprintf(stderr, "  --mcp            Start MCP server on stdio (JSON-RPC 2.0)\n");
    fprintf(stderr, "  --mcp-token-file PATH  MCP HTTP bearer token file (mode 0600)\n");
    fprintf(stderr, "  --mcp-public-url URL   Accepted exact NIP-98 /mcp URL (repeatable)\n");
    fprintf(stderr, "  --mcp-allow-npub NPUB  MCP HTTP authorized signer (repeatable)\n");
    fprintf(stderr, "  --mcp-allow-file PATH  MCP HTTP npub allowlist (one per line)\n");
    fprintf(stderr, "  --mcp-allow-host-list  Also authorize NIP-98 signers granted 'agent' by the saved\n"
                    "                   host role's live metadesk-allowlist (uses --relay);\n"
                    "                   action tools also need their 'input' grant\n");
    fprintf(stderr, "  --mcp-http [PORT] Start MCP HTTP+SSE server (default: 7710)\n");
    fprintf(stderr, "  --mcp-http-bind ADDR  HTTP listen address (default: ::1)\n");
    fprintf(stderr, "  --no-input       Disable input injection and omit MCP action tools\n");
    fprintf(stderr, "  --input-backend auto|uinput|wayland  Explicit input backend (default: auto)\n");
    fprintf(stderr, "  --input-wayland-display PATH  Absolute private Wayland socket; no ambient display\n");
    fprintf(stderr, "  --mcp-tree-format=json|compact  UI tree resource only (default: compact); action results always JSON\n");
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

static uint64_t host_fips_metric_u64(cJSON *family, const char *key) {
    cJSON *item = family ? cJSON_GetObjectItemCaseSensitive(family, key) : NULL;
    return cJSON_IsNumber(item) && item->valuedouble >= 0.0
         ? (uint64_t)item->valuedouble : 0u;
}

static void host_print_fips_metrics_summary(const char *socket_path, uint32_t timeout_ms) {
    MdFipsControlResponse metrics;
    md_fips_control_response_init(&metrics);
    MdFipsControlResult r = md_fips_control_request(socket_path, "show_metrics",
                                                    NULL, timeout_ms, &metrics);
    if (r != MD_FIPS_CONTROL_OK) {
        fprintf(stderr, "host: FIPS metrics unavailable: %s%s%s\n",
                md_fips_control_result_string(r),
                metrics.message ? ": " : "",
                metrics.message ? metrics.message : "");
        md_fips_control_response_free(&metrics);
        return;
    }

    cJSON *forwarding = metrics.data ? cJSON_GetObjectItemCaseSensitive(metrics.data, "forwarding") : NULL;
    cJSON *discovery = metrics.data ? cJSON_GetObjectItemCaseSensitive(metrics.data, "discovery") : NULL;
    cJSON *errors = metrics.data ? cJSON_GetObjectItemCaseSensitive(metrics.data, "errors") : NULL;

    printf("host: FIPS metrics forwarding sent=%llu recv=%llu discovery sent=%llu recv=%llu errors=%llu\n",
           (unsigned long long)host_fips_metric_u64(forwarding, "packets_sent"),
           (unsigned long long)host_fips_metric_u64(forwarding, "packets_recv"),
           (unsigned long long)host_fips_metric_u64(discovery, "sent"),
           (unsigned long long)host_fips_metric_u64(discovery, "recv"),
           (unsigned long long)host_fips_metric_u64(errors, "total"));
    md_fips_control_response_free(&metrics);
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

    /*
     * `state` is an opaque diagnostic string from FIPS' format!("{}", node.state()).
     * Do not branch on it. Future readiness checks should use stable documented
     * fields like tun_state, peer_count, or structured show_peers/show_sessions data.
     */
    cJSON *state = resp.data ? cJSON_GetObjectItemCaseSensitive(resp.data, "state") : NULL;
    cJSON *npub = resp.data ? cJSON_GetObjectItemCaseSensitive(resp.data, "npub") : NULL;
    printf("host: FIPS daemon ready");
    if (resp.socket_path) printf(" (%s)", resp.socket_path);
    if (cJSON_IsString(state) && state->valuestring) printf(" state=%s", state->valuestring);
    if (cJSON_IsString(npub) && npub->valuestring) printf(" npub=%.*s...", 12, npub->valuestring);
    printf("\n");
    md_fips_control_response_free(&resp);

    host_print_fips_metrics_summary(socket_path, timeout_ms);
    return 0;
}

/* ── MCP lifecycle ───────────────────────────────────────────── */

static int host_run_mcp(bool stdio_mode, bool http_mode, uint16_t port,
                        const char *bind_addr, bool no_input,
                        const MdInputConfig *input_options,
                        MdMcpTreeFormat tree_format,
                        const MdMcpBridgeConfig *http_auth,
                        const char *const *acl_relays, int acl_relay_count) {
    int rc = 1;
    MdA11yCtx *a11y = md_a11y_create();
    MdInput *input = NULL;
    MdMcpBridge *bridge = NULL;
    HostMcpAcl *acl = NULL;
    if (!md_a11y_is_connected(a11y)) {
        fprintf(stderr, "host[mcp]: accessibility backend unavailable\n");
        goto cleanup;
    }

    if (!no_input) {
        uint32_t width = 0, height = 0;
        host_apply_fallback_dimensions(&width, &height,
                                       "MCP mode has no capture pipeline");
        MdInputConfig input_cfg = *input_options;
        input_cfg.screen_width = width;
        input_cfg.screen_height = height;
        input = md_input_create(&input_cfg);
        if (!md_input_is_ready(input))
            fprintf(stderr, "host[mcp]: input unavailable; action tools disabled\n");
    }
    MdMcpBridgeConfig cfg = {
        .a11y = a11y,
        .input = input,
        .tree_format = tree_format,
        .settle_ms = 100,
        .stdio_in_fd = stdio_mode ? STDIN_FILENO : -1,
        .stdio_out_fd = stdio_mode ? STDOUT_FILENO : -1,
        .http_bind_addr = bind_addr,
        .http_port = http_mode ? (port ? port : MD_MCP_HTTP_DEFAULT_PORT) : 0,
        .http_token_file = http_auth->http_token_file,
        .http_public_urls = http_auth->http_public_urls,
        .http_public_url_count = http_auth->http_public_url_count,
        .http_allow_npubs = http_auth->http_allow_npubs,
        .http_allow_npub_count = http_auth->http_allow_npub_count,
        .http_allow_file = http_auth->http_allow_file,
    };
    /* Optional live NIP-98 source; outlives the bridge's HTTP handlers. */
    if (acl_relays) {
        acl = host_mcp_acl_start(acl_relays, acl_relay_count);
        if (!acl) goto cleanup;
        cfg.http_nip98_grant = host_mcp_acl_grant;
        cfg.http_nip98_grant_userdata = acl;
    }
    bridge = md_mcp_bridge_create(&cfg);
    if (!bridge) {
        fprintf(stderr, "host[mcp]: failed to create MCP bridge\n");
        goto cleanup;
    }

    fprintf(stderr, "host[mcp]: serving on %s\n", stdio_mode ? "stdio" : "HTTP");
    rc = md_mcp_bridge_run_supervised(bridge) < 0 ? 1 : 0;

cleanup:
    md_mcp_bridge_destroy(bridge);
    host_mcp_acl_stop(acl);
    if (input) md_input_destroy(input);
    if (a11y) md_a11y_destroy(a11y);
    return rc;
}

/* ── Main ────────────────────────────────────────────────────── */

/* Acceptance is the policy boundary, not the earlier membership prefilter.
 * Read the latest winning grant immediately before recording negotiation. */
static int host_accept_request(HostCtx *ctx, MdNostr *nostr,
                               const char *session_id, bool no_input) {
    uint32_t granted = md_nostr_granted_capabilities(nostr,
        ctx->pending_client_pk, ctx->pending_req.capabilities);
    if (no_input) granted &= ~MD_CAP_INPUT;
    if (!granted) return -1;
    if (md_session_request(ctx->session, ctx->pending_client_pk,
            ctx->pending_req.capabilities, ctx->pending_req.tree_format)) return -1;
    return md_session_accept(ctx->session, session_id, granted);
}

int main(int argc, char **argv) {
    /* Isolated metadata CLI: no capture, session, or Nostr startup. */
    if (argc > 1 && strcmp(argv[1], "account") == 0) {
        extern int md_host_account_cli(int argc, char **argv);
        return md_host_account_cli(argc - 2, argv + 2);
    }
    if (argc > 1 && strcmp(argv[1], "access") == 0) {
        extern int md_host_access_cli(int argc, char **argv);
        return md_host_access_cli(argc - 2, argv + 2);
    }
    /* Agent-side shim: signer + HTTP only, no capture/a11y/input/relay. */
    if (argc > 1 && strcmp(argv[1], "mcp-proxy") == 0)
        return md_host_mcp_proxy_cli(argc - 2, argv + 2);
    /* Every subsequently created thread inherits this mask. MCP consumes
     * termination signals with sigwait, never an async-signal-unsafe handler. */
    sigset_t term_signals;
    sigemptyset(&term_signals);
    sigaddset(&term_signals, SIGINT);
    sigaddset(&term_signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &term_signals, NULL) != 0) {
        fprintf(stderr, "ERROR: cannot block termination signals\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    /* Default options */
    uint16_t port = MD_STREAM_PORT;
    const char *bind_addr = NULL;
    const char *fips_npub = NULL;  /* expected client npub (FIPS auth) */
    const char *bunker_uri = NULL;
    const char *socket_path = NULL;
    const char *fips_control_socket = NULL;
    bool use_dbus_signer = false;
    bool auto_signer = false;
    bool use_socket_signer = false;
    uint32_t fps = 60;
    uint32_t bitrate = MD_ENCODER_DEFAULT_BITRATE;
    bool use_nvenc = true;
    bool do_capture = true;
    /* Capture policy options: keep this block independent of input selection. */
    bool capture_unattended = false;
    bool capture_forget_on_stall = false;
    uint32_t capture_consent_timeout_ms = 0;
    bool mcp_stdio = false;
    bool mcp_http = false;
    const char *mcp_token_file = NULL, *mcp_allow_file = NULL;
    const char *mcp_public_urls[32], *mcp_allow_npubs[32];
    size_t mcp_public_url_count = 0, mcp_allow_npub_count = 0;
    bool mcp_allow_host_list = false;
    bool no_input = false;
    MdInputConfig input_options = {0};
    bool signaling_option = false;
    const char *mcp_http_bind = NULL;
    MdMcpTreeFormat mcp_tree_format = MD_MCP_TREE_FORMAT_DEFAULT;
    uint16_t mcp_http_port = 0;  /* 0 = default (7710) */
    uint32_t fips_ready_timeout_ms = 10000;
    const char *relay_urls[16];
    int relay_count = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--npub") == 0 ||
            strcmp(argv[i], "--bunker") == 0 || strcmp(argv[i], "--dbus-signer") == 0 ||
            strcmp(argv[i], "--socket-signer") == 0 || strcmp(argv[i], "--auto-signer") == 0 ||
            strcmp(argv[i], "--fips-control") == 0 || strcmp(argv[i], "--fips-ready-timeout") == 0)
            signaling_option = true;
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
        /* Capture policy: portal-required on Linux, never direct fallback. */
        else if (strcmp(argv[i], "--capture-unattended") == 0)
            capture_unattended = true;
        else if (strcmp(argv[i], "--capture-forget-on-stall") == 0)
            capture_forget_on_stall = true;
        else if (strcmp(argv[i], "--capture-consent-timeout-ms") == 0) {
            if (++i >= argc || !argv[i][0]) {
                fprintf(stderr, "ERROR: capture consent timeout requires 1..180000 ms\n");
                return 1;
            }
            char *end = NULL;
            unsigned long ms = strtoul(argv[i], &end, 10);
            if (*end || ms < 1 || ms > 180000) {
                fprintf(stderr, "ERROR: capture consent timeout requires 1..180000 ms\n");
                return 1;
            }
            capture_consent_timeout_ms = (uint32_t)ms;
        }
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
            use_socket_signer = true;
            socket_path = (i + 1 < argc && argv[i + 1][0] != '-')
                ? argv[++i] : NULL;
        }
        else if (strcmp(argv[i], "--auto-signer") == 0)
            auto_signer = true;
        else if (strcmp(argv[i], "--relay") == 0 && i + 1 < argc) {
            if (relay_count < 16)
                relay_urls[relay_count++] = argv[++i];
        }
        /* Wayland input options: kept together, independent of capture flags. */
        else if (strcmp(argv[i], "--input-backend") == 0 ||
                 strcmp(argv[i], "--input-wayland-display") == 0) {
            bool backend = strcmp(argv[i], "--input-backend") == 0;
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "Missing input option value\n");
                return 1;
            }
            if (backend) input_options.backend = argv[++i];
            else input_options.wayland_display = argv[++i];
        }
        else if (strcmp(argv[i], "--no-input") == 0)
            no_input = true;
        else if (strcmp(argv[i], "--mcp-http-bind") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "Missing --mcp-http-bind address\n");
                return 1;
            }
            mcp_http_bind = argv[++i];
        }
        else if (strcmp(argv[i], "--mcp") == 0)
            mcp_stdio = true;
        else if (strncmp(argv[i], "--mcp-tree-format=", 18) == 0 ||
                 strcmp(argv[i], "--mcp-tree-format") == 0) {
            const char *format = argv[i][17] == '=' ? argv[i] + 18
                : (i + 1 < argc ? argv[++i] : "");
            if (strcmp(format, "json") == 0)
                mcp_tree_format = MD_MCP_TREE_FORMAT_JSON;
            else if (strcmp(format, "compact") == 0)
                mcp_tree_format = MD_MCP_TREE_FORMAT_COMPACT;
            else {
                fprintf(stderr, "Invalid --mcp-tree-format: expected json or compact\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--mcp-token-file") == 0 && i + 1 < argc)
            mcp_token_file = argv[++i];
        else if (strcmp(argv[i], "--mcp-allow-file") == 0 && i + 1 < argc)
            mcp_allow_file = argv[++i];
        else if (strcmp(argv[i], "--mcp-allow-host-list") == 0)
            mcp_allow_host_list = true;
        else if (strcmp(argv[i], "--mcp-public-url") == 0 && i + 1 < argc) {
            if (mcp_public_url_count == 32) {
                fprintf(stderr, "Too many --mcp-public-url values (max 32)\n");
                return 1;
            }
            mcp_public_urls[mcp_public_url_count++] = argv[++i];
        }
        else if (strcmp(argv[i], "--mcp-allow-npub") == 0 && i + 1 < argc) {
            if (mcp_allow_npub_count == 32) {
                fprintf(stderr, "Too many --mcp-allow-npub values (max 32)\n");
                return 1;
            }
            mcp_allow_npubs[mcp_allow_npub_count++] = argv[++i];
        }
        else if (strcmp(argv[i], "--mcp-http") == 0) {
            mcp_http = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                mcp_http_port = (uint16_t)atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    if ((input_options.backend && strcmp(input_options.backend, "auto") &&
         strcmp(input_options.backend, "uinput") && strcmp(input_options.backend, "wayland")) ||
        (input_options.backend && !strcmp(input_options.backend, "wayland") &&
         (!input_options.wayland_display || input_options.wayland_display[0] != '/')) ||
        (input_options.wayland_display &&
         (!input_options.backend || strcmp(input_options.backend, "wayland")))) {
        fprintf(stderr, "Invalid input selection: wayland requires an absolute --input-wayland-display path\n");
        return 1;
    }

    if ((mcp_stdio && mcp_http) || (mcp_http_bind && !mcp_http) ||
        ((mcp_stdio || mcp_http) && signaling_option)) {
        fprintf(stderr, "ERROR: MCP requires one transport; --mcp-http-bind requires HTTP; "
                        "MCP cannot use video-session Nostr/FIPS/signer options\n");
        return 1;
    }
    /* In MCP mode a relay serves only the host-allowlist NIP-98 source. */
    if ((mcp_allow_host_list && !mcp_http) ||
        ((mcp_stdio || mcp_http) && relay_count && !mcp_allow_host_list)) {
        fprintf(stderr, "ERROR: --mcp-allow-host-list requires --mcp-http; in MCP mode "
                        "--relay requires --mcp-allow-host-list\n");
        return 1;
    }

    /* MCP does not use video session negotiation, Nostr or its session log.
     * Enter before allocating them, and reserve stdout for JSON-RPC. */
    if (mcp_stdio || mcp_http) {
        const MdMcpBridgeConfig http_auth = {
            .http_token_file = mcp_token_file,
            .http_public_urls = mcp_public_urls,
            .http_public_url_count = mcp_public_url_count,
            .http_allow_npubs = mcp_allow_npubs,
            .http_allow_npub_count = mcp_allow_npub_count,
            .http_allow_file = mcp_allow_file,
        };
        if (mcp_allow_host_list && relay_count == 0)
            relay_urls[relay_count++] = MD_DEFAULT_RELAY;
        return host_run_mcp(mcp_stdio, mcp_http, mcp_http_port,
                            mcp_http_bind, no_input, &input_options, mcp_tree_format,
                            &http_auth, mcp_allow_host_list ? relay_urls : NULL,
                            relay_count);
    }

    /* Video mode only: progress and readiness lines must reach a pipe
     * (container log, supervisor) when printed. MCP returned above with
     * stdout untouched; its stdio transport owns fd 1 via write(2). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    install_signal_handlers();
    pthread_sigmask(SIG_UNBLOCK, &term_signals, NULL);

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
    MdAccountStartup account = {0};
    bool explicit_signer = bunker_uri || use_dbus_signer || use_socket_signer || auto_signer;
    if (!explicit_signer) {
        int rc = md_account_startup_open(MD_ACCOUNT_HOST, &account);
        if (rc) {
            fprintf(stderr, "ERROR: host account restore: %s\n", md_accounts_error_string(rc));
            return 1;
        }
        signer = md_signer_ref(md_account_binding_signer(account.binding));
    } else if (bunker_uri) {
        printf("host: connecting to NIP-46 bunker...\n");
        signer = md_signer_create_nip46(bunker_uri, 30000);
    } else if (use_dbus_signer) {
        printf("host: connecting to NIP-55L D-Bus signer...\n");
        signer = md_signer_create_nip55l();
    } else if (use_socket_signer || auto_signer) {
        if (use_socket_signer) {
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
    } else if (explicit_signer) {
        fprintf(stderr, "ERROR: requested signer backend not available\n");
        return 1;
    }
    if (fips_npub && !signer) {
        fprintf(stderr, "ERROR: host --npub requires a signer for Nostr session authorization\n");
        fprintf(stderr, "  Bind a host account or use --auto-signer, --socket-signer, --dbus-signer, or --bunker.\n");
        return 1;
    }

    char expected_client_pk[65] = {0};
    if (fips_npub) {
        if (md_fips_npub_to_pubkey_hex(fips_npub, expected_client_pk,
                                       sizeof(expected_client_pk)) != 0) {
            fprintf(stderr, "ERROR: --npub must be a valid bech32 FIPS/Nostr npub identity\n");
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
            return 1;
        }

        if (!bind_addr)
            printf("host: FIPS mode — accepting connections via fips0 TUN\n");

        printf("host: checking FIPS daemon health...\n");
        if (host_check_fips_daemon(fips_control_socket, 5000) != 0) {
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
            return 1;
        }
        /* Compare our checked host identity, not the expected peer, with the
         * actual daemon's show_status identity. Missing identity fails closed. */
        MdFipsControlResponse status;
        md_fips_control_response_init(&status);
        MdFipsControlResult result = md_fips_control_request(fips_control_socket,
            "show_status", NULL, 5000, &status);
        cJSON *node = status.data ? cJSON_GetObjectItemCaseSensitive(status.data, "npub") : NULL;
        char node_pk[65] = {0};
        int rc = MD_ACCOUNTS_INVALID;
        if (result == MD_FIPS_CONTROL_OK && cJSON_IsString(node) &&
            !md_fips_npub_to_pubkey_hex(node->valuestring, node_pk, sizeof(node_pk))) {
            if (account.binding) rc = md_accounts_check_fips_host(account.accounts, node_pk);
            else {
                char *host_pk = NULL;
                rc = md_signer_check_connection(signer);
                if (!rc) rc = md_signer_get_pubkey(signer, &host_pk);
                if (!rc && (!host_pk || strcmp(host_pk, node_pk))) rc = MD_ACCOUNTS_FIPS_KEY_MISMATCH;
                free(host_pk);
            }
        }
        md_fips_control_response_free(&status);
        if (rc) {
            fprintf(stderr, "ERROR: FIPS host identity check refused: %s (requires show_status npub matching host signer)\n",
                    md_accounts_error_string(rc));
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
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
            relay_urls[0] = MD_DEFAULT_RELAY;
            relay_count = 1;
        }

        MdNostrCallbacks nostr_cbs = { 0 };
        nostr_cbs.on_dm = host_on_dm;
        nostr_cbs.dm_userdata = &ctx;
        nostr_cbs.on_owner_revoked = host_owner_revoked;
        nostr_cbs.owner_userdata = &ctx;
        /* Callbacks may fire during create. */
        ctx.session_req_ch = go_channel_create(1);

        MdNostrConfig nostr_cfg = {
            .signer = signer,
            .owner = account.binding,
            .relay_urls = relay_urls,
            .relay_count = relay_count,
        };

        nostr = md_nostr_create(&nostr_cfg, &nostr_cbs);
        if (nostr) {
            ctx.nostr = nostr;

            /* Wire nostr into session log for relay publishing */
            if (session_log)
                md_session_log_set_nostr(session_log, nostr);

            /* Subscribe to allowlist updates */
            md_nostr_refresh_allowlist(nostr);

            /* Only live (post-EOSE) requests are answered; announce readiness
             * at the catch-up -> live boundary, not before it. */
            if (md_nostr_wait_history(nostr, 10000) != 0)
                fprintf(stderr, "host: WARNING: relay history catch-up (EOSE) not "
                        "confirmed; answering live session requests only\n");

            printf("host: nostr bridge ready, waiting for session requests...\n");

            /* Wait for a session request DM (event-driven with timeout) */
            {
                void *dummy = NULL;
                GoSelectCase cases[] = {
                    { .op = GO_SELECT_RECEIVE, .chan = ctx.session_req_ch, .recv_buf = &dummy },
                };
                go_select_timeout(cases, 1, 60000); /* 60s timeout */
            }

            if (ctx.session_requested && g_running &&
                (!account.binding || md_account_binding_is_current(account.binding))) {
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
                        goto signaling_failed;
                    }
                    printf("host: FIPS peer ready: %s\n", ready.detail);
                }

                /* Generate cryptographically random session ID */
                char session_id[64];
                {
                    uint8_t rnd[16];
                    if (RAND_bytes(rnd, sizeof(rnd)) != 1) {
                        fprintf(stderr, "ERROR: session entropy unavailable\n");
                        goto signaling_failed;
                    }
                    snprintf(session_id, sizeof(session_id),
                             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                             rnd[0],rnd[1],rnd[2],rnd[3], rnd[4],rnd[5],
                             rnd[6],rnd[7], rnd[8],rnd[9],
                             rnd[10],rnd[11],rnd[12],rnd[13],rnd[14],rnd[15]);
                }

                /* Record the current per-client grant now; reserve/publish
                 * its token only after listener/backends are ready. */
                if (host_accept_request(&ctx, nostr, session_id, no_input)) {
                    fprintf(stderr, "ERROR: current client policy grants no requested capabilities\n");
                    goto signaling_failed;
                }

            } else if (g_running) {
                if (fips_npub || account.binding) {
                    fprintf(stderr, "ERROR: timed out waiting for authorized host-role session request; refusing direct TCP fallback\n");
                    goto signaling_failed;
                }
                fprintf(stderr, "ERROR: no negotiated session request\n");
            }
        } else {
            if (fips_npub || account.binding) {
                fprintf(stderr, "ERROR: nostr bridge creation failed; authorized hosting cannot fall back to direct TCP\n");
                goto signaling_failed;
            }
            fprintf(stderr, "ERROR: nostr bridge creation failed\n");
        }
    }

    if (!g_running || session.state != MD_SESSION_NEGOTIATING) {
        fprintf(stderr, "ERROR: negotiated stream authorization required; direct TCP disabled\n");
        goto signaling_failed;
    }
    goto signaling_ready;
signaling_failed:
    md_nostr_destroy(nostr);
    if (ctx.session_req_ch) {
        go_channel_close(ctx.session_req_ch);
        go_channel_unref(ctx.session_req_ch);
    }
    md_session_log_destroy(session_log);
    md_signer_destroy(signer);
    md_account_startup_clear(&account);
    pthread_mutex_destroy(&ctx.client_mu);
    return 1;
signaling_ready:
    ;
    /* Create TCP server */
    MdStreamServer *srv = md_stream_server_create(bind_addr, port);
    if (!srv) {
        fprintf(stderr, "ERROR: failed to bind TCP server on port %u\n", port);
        return 1;
    }
    printf("host: listening on port %u\n", port);

    /* Start capture first so encoder/input dimensions match the negotiated
     * capture size instead of assuming a fixed desktop resolution. */
    MdCaptureCtx *capture = NULL;
    pthread_t cap_thread = 0;
    bool cap_thread_started = false;
    CaptureThread cap_thr_ctx = {0};
    uint32_t display_width = 0;
    uint32_t display_height = 0;

    if (do_capture) {
        MdCaptureConfig cap_cfg = {
            .target_fps  = fps,
            .show_cursor = true,
            .unattended = capture_unattended,
            .forget_authorization_on_stall = capture_forget_on_stall,
            .consent_timeout_ms = capture_consent_timeout_ms,
        };
        capture = md_capture_create(&cap_cfg);
        if (!capture) {
            fprintf(stderr, "WARNING: screen capture unavailable\n");
        } else if (md_capture_start(capture) < 0) {
            fprintf(stderr, "WARNING: failed to start capture\n");
            md_capture_destroy(capture);
            capture = NULL;
        } else if (host_wait_capture_dimensions(capture,
                                                &display_width,
                                                &display_height) == 0) {
            printf("host: capture started (%ux%u)\n",
                   display_width, display_height);
        } else {
            host_apply_fallback_dimensions(&display_width, &display_height,
                                           "capture dimensions not negotiated");
            printf("host: capture started\n");
        }
    }

    if (do_capture && capture_unattended && !capture) {
        fprintf(stderr, "ERROR: unattended capture authorization/start failed\n");
        return 1;
    }

    if (!host_dimensions_are_valid(display_width, display_height)) {
        host_apply_fallback_dimensions(&display_width, &display_height,
                                       do_capture ? "capture unavailable"
                                                  : "capture disabled");
    }

    /* Initialize encoder (persistent across reconnections).
     * NV12/H.264 need even dimensions; a display with an odd edge (e.g. a
     * 1512x909 VM screen) is cropped by one row/column at encode time, while
     * input injection keeps the true display size. */
    uint32_t enc_width  = display_width  & ~1u;
    uint32_t enc_height = display_height & ~1u;
    MdEncoderConfig enc_cfg = {
        .width        = enc_width,
        .height       = enc_height,
        .bitrate      = bitrate,
        .fps          = fps,
        .prefer_nvenc = use_nvenc,
    };

    MdEncoder *encoder = md_encoder_create(&enc_cfg);
    if (!encoder) {
        fprintf(stderr, "ERROR: failed to create encoder\n");
        if (capture) md_capture_destroy(capture);
        md_stream_server_destroy(srv);
        return 1;
    }
    printf("host: encoder ready (%s, %ux%u)\n",
           md_encoder_is_hw(encoder) ? "NVENC" : "x264",
           enc_width, enc_height);

    /* Initialize input injection (persistent) */
    MdInputConfig input_cfg = input_options;
    input_cfg.screen_width = display_width;
    input_cfg.screen_height = display_height;
    MdInput *input = no_input ? NULL : md_input_create(&input_cfg);
    if (!input || !md_input_is_ready(input)) {
        fprintf(stderr, "WARNING: input injection unavailable\n");
    } else {
        printf("host: input injection ready (%ux%u)\n",
               display_width, display_height);
    }

    /* Initialize accessibility tree walker (persistent) */
    MdA11yCtx *a11y = md_a11y_create();
    if (!md_a11y_is_connected(a11y)) {
        fprintf(stderr, "WARNING: accessibility tree unavailable — agent mode disabled\n");
        md_a11y_destroy(a11y);
        a11y = NULL;
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
    ctx.input   = input;  /* raw human input injection for MD_PKT_INPUT */

    /* Capture thread starts only after encoder/input are ready. */
    if (capture) {
        cap_thr_ctx.host    = &ctx;
        cap_thr_ctx.capture = capture;
        if (pthread_create(&cap_thread, NULL, capture_thread_func,
                           &cap_thr_ctx) == 0) {
            cap_thread_started = true;
        } else {
            fprintf(stderr, "WARNING: failed to start capture thread\n");
        }
    }

    /* No remote client can start its bind/ACK wait before this point. */
    int exit_status = 0;
    if (host_publish_session_accept(&ctx, account.binding) != 0) {
        fprintf(stderr, "ERROR: session acceptance failed or host role revoked\n");
        exit_status = 1;
        g_running = 0; /* Use the normal backend cleanup below. */
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

        /* A candidate never reaches capture/tree/dispatch. Failed and reused
         * proofs close only that candidate, without replacing the active one. */
        if (host_bind_candidate(&ctx, client) != 0) {
            fprintf(stderr, "host: rejected unproven client #%u\n", client_num);
            md_stream_destroy(client);
            continue;
        }

        /* Reset and publish only after the authoritative bind ACK. Serialize
         * all host sends with capture so headers/payloads cannot interleave. */
        pthread_mutex_lock(&ctx.client_mu);
        ctx.client     = client;
        ctx.pkt_seq    = 0;
        ctx.frames_sent = 0;
        ctx.total_encode_us = 0;
        ctx.frames_encoded  = 0;
        ctx.frame_size_mismatch_warned = false;

        /* The encoder is persistent across connections, so a new client joins
         * mid-GOP with no decodable entry point. Force an IDR for it. */
        if (encoder)
            md_encoder_force_keyframe(encoder);

        /* Send initial UI tree to new client */
        host_send_initial_tree(&ctx);

        pthread_mutex_unlock(&ctx.client_mu);

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
            /* Never leave a second connection waiting to inherit this grant. */
            MdStream *extra = md_stream_server_accept(srv, 1);
            if (extra) {
                host_bind_candidate(&ctx, extra);
                md_stream_destroy(extra);
            }
            /* Check for incoming packets from client (non-blocking) */
            MdPacketHeader hdr;
            uint8_t *payload = NULL;
            int ret = md_stream_recv(client, &hdr, &payload, 10 /* 10ms poll */);

            if (ret == 0) {
                pthread_mutex_lock(&ctx.client_mu);
                handle_client_packet(&ctx, &hdr, payload);
                pthread_mutex_unlock(&ctx.client_mu);
                free(payload);
            } else if (ret < 0) {
                /* Connection lost */
                break;
            }
            /* ret == 1: timeout, no data — continue */

            uint32_t now = md_stream_now_ms();

            /* Send periodic ping for RTT measurement */
            if (now - last_ping_ms >= 1000) {
                pthread_mutex_lock(&ctx.client_mu);
                md_stream_send_ping(client);
                pthread_mutex_unlock(&ctx.client_mu);
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

        /* Do NOT flush the encoder here: md_encoder_flush() sends an EOF to the
         * persistent encoder, which then refuses further frames and leaves the
         * next client with frames=0. Any buffered packets would only go to the
         * client that just left. The next client gets a forced keyframe above. */

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

    md_nostr_destroy(nostr);
    if (ctx.session_req_ch) {
        go_channel_close(ctx.session_req_ch);
        go_channel_unref(ctx.session_req_ch);
    }
    md_session_log_destroy(session_log);
    md_signer_destroy(signer);
    md_account_startup_clear(&account);

    pthread_mutex_destroy(&ctx.client_mu);

    printf("host: done. served %u client(s)\n", client_num);
    return exit_status;
}
