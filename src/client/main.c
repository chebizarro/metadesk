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
#include <openssl/crypto.h>
#include "packet.h"
#include "stream.h"
#include "decode.h"
#include "render.h"
#include "video_geometry.h"
#include "rates.h"
#include "signer.h"
#include "nostr.h"
#include "a11y.h"
#include "ui.h"
#include "packet.h"
#include "input_event.h"
#include "md_socket.h"
#include "md_time.h"

#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <go.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#endif

static atomic_int g_running = 1;
static GoChannel *g_shutdown_ch;
static pthread_t g_signal_thread;
#ifndef _WIN32
static sigset_t g_signal_set, g_previous_mask;
#else
static HANDLE g_signal_event;
static void signal_handler(int sig) {
    (void)sig;
    SetEvent(g_signal_event);
}
#endif

/* Channel operations take locks: never call them from a POSIX handler. Block
 * before starting workers; sigwait delivers shutdown on an ordinary thread. */
static void *signal_waiter(void *unused) {
    (void)unused;
#ifndef _WIN32
    int sig;
    if (sigwait(&g_signal_set, &sig) != 0) return NULL;
#else
    WaitForSingleObject(g_signal_event, INFINITE);
#endif
    g_running = 0;
    go_channel_close(g_shutdown_ch);
    return NULL;
}

static void stop_signal_wake(void) {
    if (!g_shutdown_ch) return;
#ifndef _WIN32
    pthread_kill(g_signal_thread, SIGTERM);
#else
    SetEvent(g_signal_event);
#endif
    pthread_join(g_signal_thread, NULL);
    go_channel_unref(g_shutdown_ch);
    g_shutdown_ch = NULL;
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &g_previous_mask, NULL);
#else
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    CloseHandle(g_signal_event);
#endif
}

static int start_signal_wake(void) {
    g_shutdown_ch = go_channel_create(1);
    if (!g_shutdown_ch) return -1;
#ifndef _WIN32
    sigemptyset(&g_signal_set);
    sigaddset(&g_signal_set, SIGINT);
    sigaddset(&g_signal_set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &g_signal_set, &g_previous_mask)) goto fail;
#else
    g_signal_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_signal_event) goto fail;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif
    if (!pthread_create(&g_signal_thread, NULL, signal_waiter, NULL)) return 0;
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &g_previous_mask, NULL);
#else
    CloseHandle(g_signal_event);
#endif
fail:
    go_channel_unref(g_shutdown_ch);
    g_shutdown_ch = NULL;
    return -1;
}

/* ── State ───────────────────────────────────────────────────── */

/* A decoded frame as input names it to the host (md-383x): its size and
 * the sequence of the video packet it was decoded from. */
typedef struct {
    uint32_t w, h, pkt_seq;
} FrameTag;

typedef struct {
    MdRenderer   *renderer;
    MdOverlay    *overlay;
    MdClientRates rates;
    bool monitor_control;
    char requested_output[32];
    MdCaptureOutputs outputs;
    MdQuality requested_quality, effective_quality;
    bool quality_supported;
    uint32_t      frames_decoded;
    uint32_t      frames_displayed;
    int64_t       total_decode_us;
    uint32_t      frame_w;              /* last decoded frame width  (remote px) */
    uint32_t      frame_h;              /* last decoded frame height (remote px) */
    FrameTag      uploaded;             /* last frame uploaded to the texture */
    FrameTag      shown;                /* last frame presented: input is mapped
                                         * onto and tagged with it            */
    MdStream     *stream;               /* active stream, for input forwarding    */
    uint32_t      input_seq;            /* seq counter for MD_PKT_INPUT packets    */

    /* UI tree state (received from host agent) */
    char         *ui_tree_json;         /* latest full UI tree JSON (owned)   */
    size_t        ui_tree_len;          /* byte length of ui_tree_json        */
    uint32_t      tree_updates;         /* count of tree/delta packets recv'd */
    uint32_t      tree_patch_failures;  /* independently throttled warnings */

    /* Nostr session negotiation state */
    char          expected_host_pk[128]; /* pubkey hex of host we're connecting to */
    MdSessionRequest pending_req;     /* current request (g_negotiation_mu) */
    int           request_pending;    /* accepting an answer to pending_req */
    char          accepted_session_id[64]; /* from session_accept DM     */
    char          tls_cert_sha256[65]; /* authenticated DM pin, never an address-derived identity */
    char          stream_token[65];       /* ephemeral DM-only proof    */
    atomic_uint   granted_caps;       /* from session_accept DM          */
    MdTreeFormat  accepted_tree_format; /* confirmed tree format          */
    atomic_int    session_accepted;   /* 1 accepted, -1 refused; under mutex */
    int64_t        request_started_us; /* conservative token lifetime anchor */
    GoChannel      *session_ch;       /* signaled on session accepted    */
    MdAccountWatch *owner_watch;      /* also guards direct-IP sessions  */
    MdAccountBinding *owner;          /* borrowed from startup until drain */

    /* Connection recovery UI state */
    atomic_int     phase;
    atomic_int     error;
    atomic_bool    cancel_connect;
    MdStream      *binding_stream;    /* borrowed under g_connect_mu */
    GoChannel     *cancel_ch;          /* closed by UI, drained before unref */
    bool           paused;            /* explicit disconnect, not process quit */
    int            reconnecting;      /* unexpected disconnect recovery  */
    uint32_t       reconnect_delay_ms; /* next retry delay for overlay     */
    _Atomic(const char *) connection_status; /* transient user-facing detail     */
} ClientCtx;

/* ── Decode callback: display frame ──────────────────────────── */

static void on_decoded(const MdDecodedFrame *frame, void *userdata) {
    ClientCtx *ctx = userdata;
    ctx->frames_decoded++;
    if (frame->width != ctx->frame_w || frame->height != ctx->frame_h) {
        ctx->frame_w = frame->width;
        ctx->frame_h = frame->height;
        printf("client: decoded frame size %ux%u\n", frame->width, frame->height);
    }

    if (ctx->renderer) {
        /* Upload only; the composite (video + overlay + single present) is
         * driven from the main loop via render_client_overlay(). */
        if (md_renderer_update_frame(ctx->renderer, frame->data,
                                     frame->width, frame->height) == 0) {
            ctx->frames_displayed++;
            /* pts was submitted as the packet sequence. */
            ctx->uploaded = (FrameTag){ frame->width, frame->height,
                                        (uint32_t)frame->pts };
        }
    }

    /* Overlay stats/rendering are driven from the main loop. */
}

/* ── Nostr callbacks for --npub session negotiation ──────────── */

static bool client_owner_current(const ClientCtx *ctx) {
    return g_running && !ctx->cancel_connect && (!ctx->owner || md_account_binding_is_current(ctx->owner));
}

static void client_owner_revoked(void *data) {
    ClientCtx *ctx = data;
    g_running = 0;
    if (ctx->session_ch) go_channel_close(ctx->session_ch);
}

/* Guards pending_req/request_pending and the accepted_* fields between the
 * DM worker and the negotiating thread. */
static pthread_mutex_t g_negotiation_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_connect_mu = PTHREAD_MUTEX_INITIALIZER;

static void on_session_dm(const char *sender_pubkey_hex, const char *content,
                          const MdNostrDmInfo *info, void *userdata) {
    ClientCtx *ctx = userdata;
    if (!ctx || !content || !sender_pubkey_hex || !info || !client_owner_current(ctx))
        return;

    /* Verify the DM came from the expected host */
    if (ctx->expected_host_pk[0] != '\0' &&
        strcmp(sender_pubkey_hex, ctx->expected_host_pk) != 0) {
        fprintf(stderr, "client: ignoring session DM from unexpected pubkey %.*s...\n",
                8, sender_pubkey_hex);
        return;
    }

    MdSessionReject reject;
    if (md_session_reject_from_json(content, &reject) == 0) {
        pthread_mutex_lock(&g_negotiation_mu);
        bool matched = ctx->request_pending && !ctx->session_accepted &&
            md_session_reject_matches_request(&reject, &ctx->pending_req,
                                               info->rumor_created_at);
        if (matched) {
            ctx->session_accepted = -1;
            ctx->request_pending = 0;
            ctx->connection_status = md_session_reject_message(&reject);
        }
        pthread_mutex_unlock(&g_negotiation_mu);
        if (matched && ctx->session_ch)
            go_channel_try_send(ctx->session_ch, (void *)(uintptr_t)1);
        return;
    }
    MdSessionAccept acc;
    if (md_session_accept_from_json(content, &acc) != 0) {
        OPENSSL_cleanse(acc.stream_token, sizeof(acc.stream_token));
        return;
    }

    /* md-6ns: only the answer to this client's current request counts.
     * Relay history (NIP-59 timestamps defeat `since`) replays acceptances
     * for earlier requests and sessions; they are ignored, idempotently. */
    pthread_mutex_lock(&g_negotiation_mu);
    bool matched = ctx->request_pending && !ctx->session_accepted &&
        md_session_accept_matches_request(&acc, &ctx->pending_req,
                                          info->rumor_created_at);
    if (matched && !acc.tls_cert_sha256[0]) {
        ctx->session_accepted = -1;
        ctx->request_pending = 0;
        ctx->connection_status = "host lacks pinned TLS 1.3; upgrade host (no plaintext fallback)";
        OPENSSL_cleanse(ctx->stream_token, sizeof(ctx->stream_token));
    } else if (matched && !acc.quality_present &&
        (ctx->pending_req.quality.max_width || ctx->pending_req.quality.max_height ||
         ctx->pending_req.quality.bitrate || ctx->pending_req.quality.fps)) {
        ctx->session_accepted = -1; ctx->request_pending = 0;
        ctx->connection_status = "host does not support requested quality; upgrade host";
    } else if (matched) {
        ctx->outputs=acc.outputs;
        for (size_t i=0;i<acc.outputs.count;i++)
            fprintf(stderr,"client: output %s %s%s\n",acc.outputs.items[i].id,acc.outputs.items[i].name,
                    !strcmp(acc.outputs.selected,acc.outputs.items[i].id) ? " (selected)" : "");
        ctx->quality_supported = acc.quality_present;
        ctx->effective_quality = acc.quality;
        memcpy(ctx->tls_cert_sha256, acc.tls_cert_sha256, sizeof(ctx->tls_cert_sha256));
        memcpy(ctx->stream_token, acc.stream_token, sizeof(ctx->stream_token));
        snprintf(ctx->accepted_session_id, sizeof(ctx->accepted_session_id),
                 "%s", acc.session_id);
        ctx->granted_caps = acc.granted;
        ctx->accepted_tree_format = acc.tree_format;
        ctx->session_accepted = 1;
        ctx->request_pending = 0;
    }
    pthread_mutex_unlock(&g_negotiation_mu);
    if (matched) {
        if (ctx->session_ch)
            go_channel_try_send(ctx->session_ch, (void *)(uintptr_t)1);
    } else if (acc.stream_token[0]) {
        fprintf(stderr, "client: ignoring session_accept not answering the current "
                "request (request_id=%.8s%s, %s)\n",
                acc.request_id[0] ? acc.request_id : "none",
                acc.request_id[0] ? "..." : "", info->live ? "live" : "history");
    }
    OPENSSL_cleanse(acc.stream_token, sizeof(acc.stream_token));
}

/* ── Usage ───────────────────────────────────────────────────── */

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [--host ADDRESS] --npub HOST_NPUB [OPTIONS]\n\n", argv0);
    fprintf(stderr, "Connect to a metadesk host and display remote desktop.\n\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  ADDRESS            Positional alias for --host (requires --npub)\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --host ADDRESS     Direct TCP endpoint (IP or hostname; port via --port)\n");
    fprintf(stderr, "  --npub HOST_NPUB   Required host identity; FIPS route unless --host is set\n");
    fprintf(stderr, "  --port PORT        Host port (default: %d)\n", MD_STREAM_PORT);
    fprintf(stderr, "  --no-display       Decode only, no SDL2 window\n");
    fprintf(stderr, "  --timeout MS       Connect/readiness timeout (default: 5000)\n");
    fprintf(stderr, "  --fips-control PATH  FIPS daemon control socket override\n");
    fprintf(stderr, "\nSigner options (choose one):\n");
    fprintf(stderr, "  --bunker URI       NIP-46 Nostr Connect bunker URI\n");
    fprintf(stderr, "  --dbus-signer      Use NIP-55L D-Bus signer daemon\n");
    fprintf(stderr, "  --socket-signer [PATH]  Use NIP-5F Unix socket signer\n");
    fprintf(stderr, "  --auto-signer      Auto-detect local signer\n");
    fprintf(stderr, "  --monitor-control Request monitor switching capability (off by default)\n");
    fprintf(stderr, "  --output id:N     Select host output at connect (requires monitor grant)\n");
    fprintf(stderr, "  --save-quality    Save requested quality per host (self-encrypted NIP-78)\n");
    fprintf(stderr, "  --max-size WxH    Session resolution cap (0 = host default)\n");
    fprintf(stderr, "  --bitrate BPS     Session bitrate ceiling (0 = host default)\n");
    fprintf(stderr, "  --fps FPS         Session frame rate ceiling (0 = host default)\n");
    fprintf(stderr, "  --relay URL        Relay URL (default: " MD_DEFAULT_RELAY ")\n");
    fprintf(stderr, "  -h, --help         Show this help\n");
}

/* ── Timing helper ───────────────────────────────────────────── */

static int64_t now_us(void) {
    return (int64_t)(md_monotonic_ns() / 1000u);
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

static void cleanup_session_nostr(MdNostr **nostr, ClientCtx *ctx) {
    /* Drain callbacks/revocation before releasing their channel. */
    if (ctx) {
        md_account_binding_unwatch(ctx->owner_watch);
        ctx->owner_watch = NULL;
    }
    if (nostr && *nostr) {
        md_nostr_destroy(*nostr);
        *nostr = NULL;
    }
    if (ctx && ctx->session_ch) {
        go_channel_close(ctx->session_ch);
        go_channel_unref(ctx->session_ch);
        ctx->session_ch = NULL;
    }
}

/* Caller holds g_negotiation_mu. */
static void reset_session_accept_state(ClientCtx *ctx) {
    if (!ctx) return;
    ctx->request_pending = 0;
    memset(&ctx->pending_req, 0, sizeof(ctx->pending_req));
    ctx->tls_cert_sha256[0] = '\0';
    ctx->accepted_session_id[0] = '\0';
    OPENSSL_cleanse(ctx->stream_token, sizeof(ctx->stream_token));
    ctx->granted_caps = 0;
    ctx->accepted_tree_format = MD_TREE_FORMAT_COMPACT;
    ctx->session_accepted = 0;
    ctx->connection_status = NULL;
}

static int negotiate_session(MdNostr *nostr, ClientCtx *ctx,
                                  const char *host_pubkey_hex,
                                  uint32_t timeout_ms,
                                  MdSessionRequest *out_req) {
    if (!nostr || !ctx || !host_pubkey_hex || !out_req || !client_owner_current(ctx))
        return -1;

    MdSessionRequest req = {
        .capabilities = MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT | (ctx->monitor_control ? MD_CAP_MONITOR : 0),
        .tree_format = MD_TREE_FORMAT_COMPACT,
        .quality = ctx->requested_quality, .quality_present = true,
    };
    strcpy(req.output,ctx->requested_output);
    const char *our_pk = md_nostr_get_npub(nostr);
    if (our_pk)
        strncpy(req.fips_addr, our_pk, sizeof(req.fips_addr) - 1);

    /* A fresh id per negotiation (including reconnects); its authenticated
     * validity window is this client's own wait for the answer. */
    uint64_t wait_ms = (uint64_t)timeout_ms * 2;
    uint64_t lifetime_s = (wait_ms + 999) / 1000;
    if (lifetime_s > MD_SESSION_REQUEST_MAX_LIFETIME_S)
        lifetime_s = MD_SESSION_REQUEST_MAX_LIFETIME_S;
    if (md_session_request_prepare(&req, (int64_t)time(NULL), (uint32_t)lifetime_s) != 0) {
        fprintf(stderr, "ERROR: failed to generate session request id\n");
        return -1;
    }

    ctx->request_started_us = now_us();
    char *req_json = md_session_request_to_json(&req);
    if (!req_json) {
        fprintf(stderr, "ERROR: failed to serialize session request\n");
        return -1;
    }

    /* Arm correlation before publishing: the answer may arrive first. */
    pthread_mutex_lock(&g_negotiation_mu);
    reset_session_accept_state(ctx);
    ctx->pending_req = req;
    ctx->request_pending = 1;
    pthread_mutex_unlock(&g_negotiation_mu);

    printf("client: sending session request DM (request_id=%.8s...)...\n", req.request_id);
    if (md_nostr_send_session_request(nostr, host_pubkey_hex, req_json) != 0) {
        fprintf(stderr, "ERROR: failed to send session request\n");
        free(req_json);
        pthread_mutex_lock(&g_negotiation_mu);
        reset_session_accept_state(ctx);
        pthread_mutex_unlock(&g_negotiation_mu);
        return -1;
    }
    free(req_json);

    /* Event-driven wait on the accept channel. A wakeup can be stale (an
     * earlier negotiation's signal), so re-check state until the deadline. */
    void *dummy = NULL;
    GoSelectCase cases[] = {
        { .op = GO_SELECT_RECEIVE, .chan = ctx->session_ch, .recv_buf = &dummy },
        { .op = GO_SELECT_RECEIVE, .chan = g_shutdown_ch, .recv_buf = &dummy },
        { .op = GO_SELECT_RECEIVE, .chan = ctx->cancel_ch, .recv_buf = &dummy },
    };
    int64_t deadline_us = now_us() + (int64_t)wait_ms * 1000;
    int accepted = 0;
    for (;;) {
        pthread_mutex_lock(&g_negotiation_mu);
        accepted = ctx->session_accepted;
        pthread_mutex_unlock(&g_negotiation_mu);
        int64_t left_us = deadline_us - now_us();
        if (accepted || left_us <= 0 || !client_owner_current(ctx))
            break;
        go_select_timeout(cases, ctx->cancel_ch ? 3 : (g_shutdown_ch ? 2 : 1), (uint64_t)(left_us + 999) / 1000);
    }
    pthread_mutex_lock(&g_negotiation_mu);
    ctx->request_pending = 0; /* late answers to this request are ignored */
    accepted = ctx->session_accepted;
    pthread_mutex_unlock(&g_negotiation_mu);

    if (!client_owner_current(ctx)) {
        fprintf(stderr, "client: session negotiation cancelled\n");
        return -1;
    }
    if (accepted < 0) {
        fprintf(stderr, "ERROR: %s\n", ctx->connection_status);
        return -1;
    }
    if (!accepted) {
        fprintf(stderr, "ERROR: timed out waiting for session accept\n");
        return -1;
    }

    printf("client: session accepted (id=%s)\n", ctx->accepted_session_id);
    *out_req = req;
    return 0;
}

static int send_session_info(MdStream *stream, ClientCtx *ctx) {
    if (!stream || !ctx || !client_owner_current(ctx))
        return -1;

    int64_t token_left_ms = (ctx->request_started_us +
        (int64_t)MD_SESSION_TOKEN_LIFETIME_MS * 1000 - now_us()) / 1000;
    if (token_left_ms <= 0) return -1;
    MdSessionAccept acc = { .tree_format = ctx->accepted_tree_format };
    strncpy(acc.session_id, ctx->accepted_session_id, sizeof(acc.session_id) - 1);
    acc.granted = ctx->granted_caps;

    memcpy(acc.stream_token, ctx->stream_token, sizeof(acc.stream_token));
    char *info_json = md_session_bind_to_json(&acc);
    OPENSSL_cleanse(acc.stream_token, sizeof(acc.stream_token));
    if (!info_json)
        return -1;

    if (!client_owner_current(ctx)) {
        OPENSSL_cleanse(info_json, strlen(info_json)); free(info_json); return -1;
    }
    int ret = md_stream_send(stream, MD_PKT_SESSION_INFO, 0,
                             (const uint8_t *)info_json,
                             (uint32_t)strlen(info_json));
    OPENSSL_cleanse(info_json, strlen(info_json));
    free(info_json);
    OPENSSL_cleanse(ctx->stream_token, sizeof(ctx->stream_token));
    if (ret != 0) return -1;

    /* Do not declare this client active on a successful send alone. */
    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    token_left_ms = (ctx->request_started_us +
        (int64_t)MD_SESSION_TOKEN_LIFETIME_MS * 1000 - now_us()) / 1000;
    if (token_left_ms <= 0 || !client_owner_current(ctx)) return -1;
    ret = md_stream_recv(stream, &hdr, &payload,
        token_left_ms < MD_SESSION_BIND_TIMEOUT_MS ? (uint32_t)token_left_ms : MD_SESSION_BIND_TIMEOUT_MS);
    if (ret != 0) return -1;
    bool valid = hdr.type == MD_PKT_SESSION_INFO && hdr.payload_len > 0 &&
        hdr.payload_len <= MD_SESSION_JSON_MAX &&
        !memchr(payload, '\0', hdr.payload_len);
    if (valid) {
        char json[MD_SESSION_JSON_MAX + 1];
        memcpy(json, payload, hdr.payload_len);
        json[hdr.payload_len] = '\0';
        MdSessionAccept ack;
        valid = md_session_accept_from_json(json, &ack) == 0 &&
            strcmp(ack.session_id, ctx->accepted_session_id) == 0 &&
            ack.granted == ctx->granted_caps &&
            ack.tree_format == ctx->accepted_tree_format && !ack.stream_token[0];
    }
    free(payload);
    return valid && client_owner_current(ctx) && now_us() < ctx->request_started_us +
        (int64_t)MD_SESSION_TOKEN_LIFETIME_MS * 1000 ? 0 : -1;
}

static void cancel_client_connect(ClientCtx *ctx) {
    if (!atomic_exchange(&ctx->cancel_connect, true) && ctx->cancel_ch)
        go_channel_close(ctx->cancel_ch);
    /* Borrow only while holding the same lock the worker uses to withdraw
     * the candidate. Close wakes bind I/O; the worker alone destroys it. */
    pthread_mutex_lock(&g_connect_mu);
    if (ctx->binding_stream)
        md_stream_close(ctx->binding_stream);
    pthread_mutex_unlock(&g_connect_mu);
}

static int bind_client_stream(MdStream *stream, ClientCtx *ctx) {
    pthread_mutex_lock(&g_connect_mu);
    if (!client_owner_current(ctx)) {
        pthread_mutex_unlock(&g_connect_mu);
        md_stream_close(stream);
        return -1;
    }
    ctx->binding_stream = stream;
    pthread_mutex_unlock(&g_connect_mu);
    int rc = send_session_info(stream, ctx);
    pthread_mutex_lock(&g_connect_mu);
    ctx->binding_stream = NULL;
    pthread_mutex_unlock(&g_connect_mu);
    return rc;
}

static int wait_client_backoff(ClientCtx *ctx, uint32_t delay_ms) {
    void *value;
    GoSelectCase cases[] = {
        { .op = GO_SELECT_RECEIVE, .chan = ctx->session_ch, .recv_buf = &value },
        { .op = GO_SELECT_RECEIVE, .chan = g_shutdown_ch, .recv_buf = &value },
        { .op = GO_SELECT_RECEIVE, .chan = ctx->cancel_ch, .recv_buf = &value },
    };
    int64_t deadline = now_us() + (int64_t)delay_ms * 1000;
    while (client_owner_current(ctx)) {
        int64_t left = deadline - now_us();
        if (left <= 0) return 0;
        go_select_timeout(cases, ctx->cancel_ch ? 3 : (g_shutdown_ch ? 2 : 1), (left + 999) / 1000);
    }
    return -1;
}

/* No readiness-event command exists in the FIPS control protocol. Use actual
 * dial outcomes and fresh control snapshots, with cancellable exponential
 * retry backoff, never a sleep-based path gate or a new session request. */
static MdStream *dial_fips_session(const char *npub, const char *control,
                                   uint16_t port, uint32_t timeout_ms,
                                   ClientCtx *ctx) {
    int64_t deadline = ctx->request_started_us +
        (int64_t)MD_SESSION_TOKEN_LIFETIME_MS * 1000;
    uint32_t backoff = 100;
    while (client_owner_current(ctx)) {
        int64_t left_ms = (deadline - now_us()) / 1000;
        if (left_ms <= 0) break;
        uint32_t budget = (uint32_t)left_ms;
        if (budget > 1000) budget = 1000;
        if (timeout_ms && budget > timeout_ms) budget = timeout_ms;
        MdFipsPeerReadiness ready;
        MdFipsPeerReadinessState state = md_fips_control_check_peer_ready(
            control, npub, budget, &ready);
        if (!client_owner_current(ctx)) break;
        left_ms = (deadline - now_us()) / 1000;
        if (left_ms <= 0) break;
        if (budget > (uint32_t)left_ms) budget = (uint32_t)left_ms;
        if (state == MD_FIPS_PEER_READY) {
            MdStream *stream = md_stream_connect_fips(npub, port, budget);
            if (stream) {
                if (now_us() < deadline && client_owner_current(ctx)) return stream;
                md_stream_destroy(stream);
                break;
            }
        } else if (state == MD_FIPS_PEER_ERROR &&
                   ready.control_result != MD_FIPS_CONTROL_DAEMON_UNAVAILABLE) {
            break; /* malformed/unsupported control state is not transient */
        }
        left_ms = (deadline - now_us()) / 1000;
        if (left_ms <= 0) break;
        uint32_t delay = backoff < (uint32_t)left_ms ? backoff : (uint32_t)left_ms;
        if (wait_client_backoff(ctx, delay) != 0) break;
        if (backoff < 2000) backoff = backoff * 2 > 2000 ? 2000 : backoff * 2;
    }
    return NULL;
}

static MdStream *connect_client_stream(const char *host, const char *npub,
                                       const char *fips_control_socket,
                                       uint16_t port, uint32_t timeout_ms,
                                       MdNostr *nostr,
                                       const char *host_pubkey_hex,
                                       ClientCtx *ctx,
                                       MdSession *session) {
    MdStream *stream = NULL;
    if (!client_owner_current(ctx)) return NULL;

    if (!npub || !nostr || !host_pubkey_hex || !host_pubkey_hex[0]) {
        fprintf(stderr, "ERROR: authenticated session requires --npub host identity\n");
        return NULL;
    }

    ctx->error = MD_CLIENT_ERROR_NONE;
    if (!host) {
        ctx->phase = MD_CLIENT_FIPS;
        printf("client: checking FIPS daemon health...\n");
        if (check_fips_daemon(fips_control_socket,
                              timeout_ms && timeout_ms < 1000 ? timeout_ms : 1000) != 0)
            return NULL;

        printf("client: waiting for FIPS peer readiness for %.*s...\n", 12, npub);
        MdFipsPeerReadiness ready = {0};
        MdFipsPeerReadinessState rstate = MD_FIPS_PEER_CONVERGING;
        int64_t deadline = now_us() + (int64_t)(timeout_ms ? timeout_ms : 5000) * 1000;
        uint32_t backoff = 100;
        while (client_owner_current(ctx)) {
            int64_t left = (deadline - now_us()) / 1000;
            if (left <= 0) break;
            rstate = md_fips_control_check_peer_ready(fips_control_socket, npub,
                (uint32_t)(left < 1000 ? left : 1000), &ready);
            if (rstate == MD_FIPS_PEER_READY || rstate == MD_FIPS_PEER_ERROR) break;
            left = (deadline - now_us()) / 1000;
            if (left <= 0 || wait_client_backoff(ctx,
                backoff < (uint32_t)left ? backoff : (uint32_t)left) != 0) break;
            if (backoff < 2000) backoff = backoff * 2 > 2000 ? 2000 : backoff * 2;
        }
        if (rstate != MD_FIPS_PEER_READY) {
            if (rstate == MD_FIPS_PEER_NOT_FOUND)
                print_fips_setup_guidance("client", npub, &ready);
            else
                fprintf(stderr, "ERROR: FIPS route not ready (%s): %s\n",
                        md_fips_peer_readiness_string(rstate), ready.detail);
            return NULL;
        }
        printf("client: FIPS peer ready: %s\n", ready.detail);

    }

    /* Endpoint selection never bypasses identity-pinned Nostr admission. */
    ctx->phase = MD_CLIENT_NEGOTIATING;
    MdSessionRequest req;
    if (negotiate_session(nostr, ctx, host_pubkey_hex,
                          timeout_ms, &req) != 0 || !client_owner_current(ctx))
        return NULL;

    if (!ctx->tls_cert_sha256[0]) {
        fprintf(stderr, "ERROR: pinned TLS 1.3 required; host acceptance has no certificate pin; upgrade host\n");
        OPENSSL_cleanse(ctx->stream_token, sizeof(ctx->stream_token));
        return NULL;
    }
    ctx->phase = MD_CLIENT_CONNECTING;
    if (host) {
        printf("client: connecting to %s:%u...\n", host, port);
        stream = md_stream_connect(host, port,
                                   timeout_ms && timeout_ms < 1000 ? timeout_ms : 1000);
    } else {
        printf("client: connecting via FIPS to %.*s...\n", 12, npub);
        stream = dial_fips_session(npub, fips_control_socket, port, timeout_ms, ctx);
    }
    if (!stream) {
        fprintf(stderr, "ERROR: %s connect failed\n", host ? "direct TCP" : "FIPS");
        return NULL;
    }
    MdStreamTlsConfig tls = { .enabled = true, .cert_sha256 = ctx->tls_cert_sha256 };
    if (md_stream_start_tls(stream, timeout_ms, &tls) != 0) {
        fprintf(stderr, "ERROR: pinned TLS 1.3 failed; no token sent, no plaintext fallback\n");
        OPENSSL_cleanse(ctx->stream_token, sizeof(ctx->stream_token));
        md_stream_destroy(stream);
        return NULL;
    }
    ctx->phase = MD_CLIENT_BINDING;
    if (!client_owner_current(ctx) || bind_client_stream(stream, ctx) != 0 ||
        !client_owner_current(ctx)) {
        fprintf(stderr, "ERROR: session binding/ACK failed\n");
        md_stream_destroy(stream);
        return NULL;
    }

    md_session_reset(session);
    md_session_request(session, npub, req.capabilities,
                       ctx->accepted_tree_format);
    md_session_accept(session, ctx->accepted_session_id, ctx->granted_caps);
    md_session_activate(session);

    if (!client_owner_current(ctx)) {
        md_stream_destroy(stream);
        return NULL;
    }
    ctx->phase = MD_CLIENT_STREAMING;
    ctx->error = MD_CLIENT_ERROR_NONE;
    return stream;
}

/* Map an SDL non-text key to a name md_input understands, or NULL. */
static const char *sdl_key_name(SDL_Keycode k) {
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return "enter";
    case SDLK_BACKSPACE: return "backspace";
    case SDLK_TAB:       return "tab";
    case SDLK_ESCAPE:    return "escape";
    case SDLK_DELETE:    return "delete";
    case SDLK_INSERT:    return "insert";
    case SDLK_SPACE:     return "space";
    case SDLK_UP:        return "up";
    case SDLK_DOWN:      return "down";
    case SDLK_LEFT:      return "left";
    case SDLK_RIGHT:     return "right";
    case SDLK_HOME:      return "home";
    case SDLK_END:       return "end";
    case SDLK_PAGEUP:    return "pageup";
    case SDLK_PAGEDOWN:  return "pagedown";
    case SDLK_F1: return "f1"; case SDLK_F2: return "f2"; case SDLK_F3: return "f3";
    case SDLK_F4: return "f4"; case SDLK_F5: return "f5"; case SDLK_F6: return "f6";
    case SDLK_F7: return "f7"; case SDLK_F8: return "f8"; case SDLK_F9: return "f9";
    case SDLK_F10: return "f10"; case SDLK_F11: return "f11"; case SDLK_F12: return "f12";
    default: return NULL;
    }
}

/* Serialize and send one input event to the host, tagged with the frame on
 * screen: the host interprets points against that frame, not one it
 * captured since (md-383x). Events are pumped before each video packet is
 * decoded, so queued input is mapped while its frame is still shown. */
static void send_input(ClientCtx *ctx, uint8_t kind, uint8_t button,
                       int16_t x, int16_t y, const char *text, uint16_t tlen) {
    if (!ctx->stream || !(ctx->granted_caps & MD_CAP_INPUT)) return;
    uint8_t buf[MD_INPUT_EVENT_MAX_LEN];
    const FrameTag *t = &ctx->shown;
    const bool fits = t->w <= UINT16_MAX && t->h <= UINT16_MAX;
    size_t n = md_input_event_pack(buf, sizeof(buf), kind, button, x, y,
                                   text, tlen, fits ? (uint16_t)t->w : 0,
                                   fits ? (uint16_t)t->h : 0, t->pkt_seq);
    if (n == 0) return;
    md_stream_send(ctx->stream, MD_PKT_INPUT, ctx->input_seq++, buf, (uint32_t)n);
}

/* Map window-point coordinates to remote frame pixels. */
static bool map_to_remote(ClientCtx *ctx, int wx, int wy,
                          int16_t *rx, int16_t *ry) {
    const uint32_t fw = ctx->shown.w, fh = ctx->shown.h;
    if (!ctx->renderer || fw == 0 || fh == 0)
        return false;
    /* Use live point and drawable sizes, as presentation does. Input in
     * letterbox/pillarbox regions must not become a click at a remote edge. */
    SDL_Window *win = (SDL_Window *)md_renderer_get_sdl_window(ctx->renderer);
    int ww = 0, wh = 0;
    if (win)
        SDL_GetWindowSize(win, &ww, &wh);
    SDL_Renderer *renderer = (SDL_Renderer *)md_renderer_get_sdl_renderer(ctx->renderer);
    int ow = 0, oh = 0;
    if (!renderer || SDL_GetRendererOutputSize(renderer, &ow, &oh) != 0)
        return false;
    uint32_t mx, my;
    if (!md_video_map(fw, fh, ww, wh, ow, oh, wx, wy, &mx, &my) ||
        mx > INT16_MAX || my > INT16_MAX)
        return false;
    *rx = (int16_t)mx;
    *ry = (int16_t)my;
    return true;
}

/* Translate an SDL input event to the remote and forward it, unless the
 * ImGui overlay is consuming it (pointer over the panel, widget focused). */
static void forward_input_event(ClientCtx *ctx, const SDL_Event *ev) {
    if (!ctx->stream)
        return;
    int16_t rx, ry;
    switch (ev->type) {
    case SDL_MOUSEMOTION:
        if (md_overlay_wants_mouse(ctx->overlay)) return;
        if (map_to_remote(ctx, ev->motion.x, ev->motion.y, &rx, &ry))
            send_input(ctx, MD_INPUT_EV_MOVE, 0, rx, ry, NULL, 0);
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (md_overlay_wants_mouse(ctx->overlay)) return;
        if (map_to_remote(ctx, ev->button.x, ev->button.y, &rx, &ry)) {
            uint8_t b = ev->button.button == SDL_BUTTON_RIGHT  ? 3 :
                        ev->button.button == SDL_BUTTON_MIDDLE ? 2 : 1;
            send_input(ctx, MD_INPUT_EV_BUTTON, b, rx, ry, NULL, 0);
        }
        break;
    case SDL_MOUSEWHEEL:
        if (md_overlay_wants_mouse(ctx->overlay)) return;
        send_input(ctx, MD_INPUT_EV_WHEEL, 0,
                   (int16_t)ev->wheel.x, (int16_t)ev->wheel.y, NULL, 0);
        break;
    case SDL_KEYDOWN: {
        if (md_overlay_wants_keyboard(ctx->overlay)) return;
        SDL_Keycode k = ev->key.keysym.sym;
        if (k == SDLK_F1 || k == SDLK_ESCAPE) return;
        char namebuf[2] = {0, 0};
        const char *name = NULL;
        if (k >= SDLK_a && k <= SDLK_z) { namebuf[0] = (char)k; name = namebuf; }
        else if (k >= SDLK_0 && k <= SDLK_9) { namebuf[0] = (char)k; name = namebuf; }
        else name = sdl_key_name(k);
        if (!name) break;
        /* Modifier bitmask: host rebuilds the key combo. Sending shift+"1"
         * lets the host produce "!", shift+"a" -> "A", etc. */
        uint8_t mods = 0;
        SDL_Keymod m = (SDL_Keymod)ev->key.keysym.mod;
        if (m & KMOD_CTRL)  mods |= 1;
        if (m & KMOD_SHIFT) mods |= 2;
        if (m & KMOD_ALT)   mods |= 4;
        if (m & KMOD_GUI)   mods |= 8;
        send_input(ctx, MD_INPUT_EV_KEY, mods, 0, 0, name, (uint16_t)strlen(name));
        break;
    }
    default:
        break;
    }
}

/* Feed every SDL event to the overlay, then forward un-consumed input. */
static void overlay_event_forward(void *event, void *userdata) {
    ClientCtx *ctx = (ClientCtx *)userdata;
    if (!md_overlay_handle_sdl_event(ctx->overlay, event))
        forward_input_event(ctx, (const SDL_Event *)event);
}

static void render_client_overlay(ClientCtx *ctx, MdStream *stream) {
    if (!ctx || !ctx->overlay)
        return;

    MdStreamStats stream_stats = { 0 };
    if (stream)
        md_stream_get_stats(stream, &stream_stats);

    MdClientRateValues rates = { 0 };
    if (stream)
        rates = md_client_rates_update(&ctx->rates, md_stream_now_ms(),
                                       ctx->frames_decoded, stream_stats.bytes_recv);
    else
        ctx->rates = (MdClientRates){ 0 };
    double avg_decode_ms = ctx->frames_decoded > 0
        ? (double)ctx->total_decode_us / ctx->frames_decoded / 1000.0
        : 0.0;

    MdOverlayStats overlay_stats = {
        .latency_ms   = (float)(avg_decode_ms + stream_stats.avg_rtt_ms),
        .encode_ms    = 0.0f,
        .decode_ms    = (float)avg_decode_ms,
        .rtt_ms       = (float)stream_stats.avg_rtt_ms,
        .phase = ctx->phase,
        .error = ctx->error,
        .granted_caps = ctx->granted_caps,
        .paused = ctx->paused,
        .connected    = stream && md_stream_is_connected(stream),
        .reconnecting = ctx->reconnecting != 0,
        .reconnect_delay_ms = ctx->reconnect_delay_ms,
        .status_message = ctx->connection_status,
        .fps          = (int)rates.fps,
        .bitrate_mbps = (float)rates.mbps,
        .encoder_name = NULL,
        .outputs = stream ? &ctx->outputs : NULL,
        .quality_supported = stream && ctx->quality_supported,
        .quality = stream ? ctx->effective_quality : (MdQuality){0},
    };

    /* One composite per displayed frame: draw the latest video texture,
     * then the overlay on top, then a single present. Drawing both into the
     * same back buffer is what stops the overlay leaving ghost trails. */
    md_renderer_begin_frame(ctx->renderer);
    md_overlay_new_frame(ctx->overlay);
    md_overlay_render(ctx->overlay, &overlay_stats);
    char output[32];
    if (stream && md_overlay_take_output(ctx->overlay,output)) {
        cJSON *root=cJSON_CreateObject();
        cJSON_AddStringToObject(root,"type","monitor"); cJSON_AddStringToObject(root,"output",output);
        char *json=cJSON_PrintUnformatted(root); cJSON_Delete(root);
        if (json) { if (md_stream_send(stream,MD_PKT_SESSION_CONTROL,ctx->input_seq++,
            (uint8_t*)json,(uint32_t)strlen(json))) md_stream_close(stream); free(json); }
    }
    MdQuality changed;
    if (stream && md_overlay_take_quality(ctx->overlay, &changed)) {
        char *json = md_quality_control(&changed);
        if (json) {
            if (md_stream_send(stream, MD_PKT_SESSION_CONTROL, ctx->input_seq++,
                               (uint8_t *)json, (uint32_t)strlen(json))) md_stream_close(stream);
            else ctx->requested_quality = changed;
            free(json);
        }
    }
    md_renderer_end_frame(ctx->renderer);
    ctx->shown = ctx->uploaded;
}

static void reset_client_video(ClientCtx *ctx) {
    ctx->stream = NULL;
    ctx->uploaded = ctx->shown = (FrameTag){0};
    ctx->frame_w = ctx->frame_h = ctx->input_seq = 0;
    ctx->rates = (MdClientRates){0};
    free(ctx->ui_tree_json);
    ctx->ui_tree_json = NULL;
    ctx->ui_tree_len = 0;
    md_renderer_reset_frame(ctx->renderer);
}

static void pump_client_ui(ClientCtx *ctx, MdStream *stream) {
    if (ctx->renderer && md_renderer_poll_events(ctx->renderer) < 0) g_running = 0;
    render_client_overlay(ctx, stream);
    MdOverlayAction action = md_overlay_take_action(ctx->overlay);
    if (action == MD_OVERLAY_QUIT) g_running = 0;
    if (action == MD_OVERLAY_DISCONNECT || !g_running) {
        ctx->paused = true;
        ctx->error = MD_CLIENT_ERROR_NONE;
        ctx->connection_status = "Disconnected by you";
        cancel_client_connect(ctx);
        ctx->stream = NULL; /* no input after explicit disconnect */
    } else if (action == MD_OVERLAY_RETRY) {
        ctx->paused = false;
    }
}

typedef struct {
    const char *host, *npub, *control, *pubkey;
    uint16_t port;
    uint32_t timeout;
    MdNostr *nostr;
    ClientCtx *ctx;
    MdSession *session;
    MdStream *result;
    GoChannel *done;
} ClientConnect;

static void *connect_worker(void *data) {
    ClientConnect *job = data;
    job->result = connect_client_stream(job->host, job->npub, job->control,
        job->port, job->timeout, job->nostr, job->pubkey, job->ctx, job->session);
    if (!job->result && client_owner_current(job->ctx)) {
        switch (job->ctx->phase) {
        case MD_CLIENT_FIPS: job->ctx->error = MD_CLIENT_ERROR_FIPS; break;
        case MD_CLIENT_NEGOTIATING: job->ctx->error = MD_CLIENT_ERROR_ADMISSION; break;
        case MD_CLIENT_BINDING: job->ctx->error = MD_CLIENT_ERROR_BINDING; break;
        default: job->ctx->error = MD_CLIENT_ERROR_TRANSPORT; break;
        }
    }
    go_channel_close(job->done);
    return NULL;
}

static MdStream *connect_with_ui(const char *host, const char *npub,
                                 const char *control, uint16_t port, uint32_t timeout,
                                 MdNostr *nostr, const char *pubkey,
                                 ClientCtx *ctx, MdSession *session) {
    ctx->cancel_connect = false;
    ctx->reconnect_delay_ms = 0;
    ctx->cancel_ch = go_channel_create(1);
    ClientConnect job = {host, npub, control, pubkey, port, timeout,
                         nostr, ctx, session, NULL, go_channel_create(1)};
    pthread_t thread;
    if (!ctx->cancel_ch || !job.done ||
        pthread_create(&thread, NULL, connect_worker, &job) != 0) {
        if (job.done) go_channel_unref(job.done);
        if (ctx->cancel_ch) go_channel_unref(ctx->cancel_ch);
        ctx->cancel_ch = NULL;
        return NULL;
    }
    void *value;
    GoSelectCase done = {.op = GO_SELECT_RECEIVE, .chan = job.done, .recv_buf = &value};
    /* 16ms is the overlay animation cadence, not a relay completion timer.
     * Cancellation closes a channel; all worker ownership drains via join. */
    while (go_select_timeout(&done, 1, 16).selected_case < 0) pump_client_ui(ctx, NULL);
    pthread_join(thread, NULL);
    go_channel_unref(job.done);
    go_channel_unref(ctx->cancel_ch);
    ctx->cancel_ch = NULL;
    if (ctx->cancel_connect || !g_running) {
        md_stream_destroy(job.result);
        job.result = NULL;
    }
    return job.result;
}

static int sleep_with_reconnect_ui(ClientCtx *ctx, uint32_t delay_ms,
                                   bool *deliberate_exit) {
    int64_t deadline = now_us() + (int64_t)delay_ms * 1000;
    ctx->phase = MD_CLIENT_BACKOFF;
    while (g_running && !ctx->paused) {
        int64_t left = (deadline - now_us()) / 1000;
        if (left <= 0) return 0;
        ctx->reconnect_delay_ms = (uint32_t)left;
        pump_client_ui(ctx, NULL);
        if (!g_running || ctx->paused) break;
        wait_client_backoff(ctx, (uint32_t)(left < 16 ? left : 16));
    }
    *deliberate_exit = !g_running;
    return -1;
}

/* ── Main ────────────────────────────────────────────────────── */

static int client_run(int argc, char **argv) {
    /* Progress lines must reach a pipe (container log, supervisor) when
     * printed, not when a block buffer fills or the process exits. */
#ifdef _WIN32
    /* UCRT treats _IOLBF as full buffering, including redirected output. */
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif
    if (start_signal_wake() != 0) return 1;
    atexit(stop_signal_wake);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);  /* host going away mid-send is EPIPE, not fatal */
#else
    SDL_SetMainReady();
#endif

    /* Default options */
    const char *host = NULL;
    const char *npub = NULL;
    const char *bunker_uri = NULL;
    const char *socket_path = NULL;
    const char *fips_control_socket = NULL;
    bool use_dbus_signer = false;
    bool auto_signer = false;
    bool use_socket_signer = false;
    uint16_t port = MD_STREAM_PORT;
    bool quality_cli = false, save_quality = false;
    bool do_display = true;
    bool monitor_control=false;
    char requested_output[32]={0};
    MdQuality requested_quality = {0};
    uint32_t timeout_ms = 5000;
    const char *relay_urls[16];
    int relay_count = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-size") == 0 && i + 1 < argc) {
            quality_cli = true;
            char tail;
            if (sscanf(argv[++i], "%ux%u%c", &requested_quality.max_width,
                       &requested_quality.max_height, &tail) != 2 ||
                !md_quality_valid(&requested_quality)) {
                fprintf(stderr, "ERROR: --max-size requires WxH, each 0 or 2..16384\n"); return 1;
            }
        }
        else if ((strcmp(argv[i], "--bitrate") == 0 || strcmp(argv[i], "--fps") == 0) && i + 1 < argc) {
            quality_cli = true;
            bool is_fps = strcmp(argv[i], "--fps") == 0;
            const char *value = argv[++i]; char *end;
            unsigned long n = strtoul(value, &end, 10);
            if (!value[0] || *end || value[0] == '-' || n > UINT32_MAX) return 1;
            if (is_fps) requested_quality.fps = (uint32_t)n;
            else requested_quality.bitrate = (uint32_t)n;
            if (!md_quality_valid(&requested_quality)) {
                fprintf(stderr, "ERROR: bitrate must be 0 or 100000..100000000; fps 0..240\n"); return 1;
            }
        }
        else if (strcmp(argv[i],"--output")==0 && i+1<argc) {
            const char *id=argv[++i];
            if (strlen(id)>=sizeof(requested_output) || !md_capture_output_id_valid(id)) {
                fprintf(stderr,"ERROR: --output requires a stable id:N selector (Windows also all)\n"); return 1;
            }
            strcpy(requested_output,id); monitor_control=true;
        }
        else if (!strcmp(argv[i],"--monitor-control")) monitor_control=true;
        else if (strcmp(argv[i], "--save-quality") == 0) save_quality = true;
        else if (strcmp(argv[i], "--no-display") == 0)
            do_display = false;
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout_ms = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--fips-control") == 0 && i + 1 < argc)
            fips_control_socket = argv[++i];
        else if (strcmp(argv[i], "--host") == 0) {
            if (host || i + 1 >= argc || argv[i + 1][0] == '-' || !argv[i + 1][0]) {
                fprintf(stderr, "ERROR: --host requires one address (port via --port)\n");
                return 1;
            }
            host = argv[++i];
        }
        else if (strcmp(argv[i], "--npub") == 0 && i + 1 < argc)
            npub = argv[++i];
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
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        else if (argv[i][0] != '-' && !host)
            host = argv[i];
        else {
            fprintf(stderr, "ERROR: unknown or incomplete option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!npub) {
        fprintf(stderr, "ERROR: --npub host identity required, including for direct TCP (--host ADDRESS)\n\n");
        usage(argv[0]);
        return 1;
    }

    printf("metadesk-client v0.1.0\n");
    if (!host) {
        printf("  fips:      %.*s...\n", 12, npub);
    } else {
        printf("  host:      %s:%u\n", host, port);
    }
    printf("  display:   %s\n", do_display ? "SDL2" : "disabled");
    printf("  timeout:   %u ms\n", timeout_ms);
    printf("\n");

    /* GUI mode needs a display a person can see. Check before any signer or
     * session work so a missing display fails fast, never as decode-only. */
    if (do_display && md_renderer_open_display() < 0) {
        fprintf(stderr, "ERROR: no usable display for the GUI client; "
                        "grant a display or use --no-display for decode-only\n");
        return 1;
    }

    /* ── Signer initialization ─────────────────────────────── */
    MdSigner *signer = NULL;
    MdAccountStartup account = {0};
    bool explicit_signer = bunker_uri || use_dbus_signer || use_socket_signer || auto_signer;
    if (!explicit_signer) {
        int rc = md_account_startup_open(MD_ACCOUNT_CLIENT, &account);
        if (rc) {
            fprintf(stderr, "ERROR: client account restore: %s\n", md_accounts_error_string(rc));
            return 1;
        }
        signer = md_signer_ref(md_account_binding_signer(account.binding));
    } else if (bunker_uri) {
        printf("client: connecting to NIP-46 bunker...\n");
        signer = md_signer_create_nip46(bunker_uri, 30000);
    } else if (use_dbus_signer) {
        printf("client: connecting to NIP-55L D-Bus signer...\n");
        signer = md_signer_create_nip55l();
    } else if (use_socket_signer || auto_signer) {
        if (use_socket_signer) {
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
    } else if (explicit_signer) {
        fprintf(stderr, "ERROR: requested signer backend not available\n");
        return 1;
    }

    /* Initialize session state */
    MdSession session;
    md_session_init(&session);

    /* Client context (needed before connect for nostr callbacks) */
    ClientCtx ctx = { .owner = account.binding, .requested_quality = requested_quality };
    ctx.monitor_control=monitor_control;
    strcpy(ctx.requested_output,requested_output);
    if (account.binding) {
        ctx.owner_watch = md_account_binding_watch(account.binding, client_owner_revoked, &ctx);
        if (!ctx.owner_watch) {
            fprintf(stderr, "ERROR: cannot watch client role generation\n");
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
            return 1;
        }
    }

    /* Connect to host */
    MdStream *stream = NULL;
    MdNostr *nostr = NULL;
    char host_pubkey_hex[65] = { 0 };

    if (npub) {
        /* Nostr authorization is shared by direct TCP and FIPS. */

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
            relay_urls[0] = MD_DEFAULT_RELAY;
            relay_count = 1;
        }

        /* Session DM callback. FIPS reachability is owned by the FIPS daemon
         * and checked through fips_control. */
        MdNostrCallbacks nostr_cbs = { 0 };
        nostr_cbs.on_dm = on_session_dm;
        nostr_cbs.dm_userdata = &ctx;

        MdNostrConfig nostr_cfg = {
            .signer = signer,
            .owner = account.binding,
            .relay_urls = relay_urls,
            .relay_count = relay_count,
            .quality_preferences = true,
        };

        if (md_fips_npub_to_pubkey_hex(npub, host_pubkey_hex, sizeof(host_pubkey_hex)) != 0) {
            fprintf(stderr, "ERROR: --npub must be a valid bech32 FIPS/Nostr npub identity\n");
            cleanup_session_nostr(&nostr, &ctx);
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
            return 1;
        }
        /* Pin expected peer before callbacks can run. */
        strncpy(ctx.expected_host_pk, host_pubkey_hex, sizeof(ctx.expected_host_pk) - 1);

        /* Create channel before nostr_create — callbacks fire on worker threads */
        ctx.session_ch = go_channel_create(1);

        nostr = md_nostr_create(&nostr_cfg, &nostr_cbs);
        if (!nostr) {
            fprintf(stderr, "ERROR: failed to create Nostr bridge\n");
            cleanup_session_nostr(&nostr, &ctx);
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
            return 1;
        }


    }

    if (nostr) {
        if (!quality_cli && md_nostr_quality_load(nostr, ctx.expected_host_pk, &ctx.requested_quality) < 0)
            fprintf(stderr, "client: saved quality unavailable or invalid; using host defaults\n");
        if (save_quality && md_nostr_quality_save(nostr, ctx.expected_host_pk, &ctx.requested_quality)) {
            fprintf(stderr, "ERROR: could not save encrypted quality preferences (requires relay OK)\n");
            cleanup_session_nostr(&nostr,&ctx); md_signer_destroy(signer); md_account_startup_clear(&account); return 1;
        }
    }
    /* Create decoder */
    MdDecoder *decoder = md_decoder_create();
    if (!decoder) {
        fprintf(stderr, "ERROR: failed to create H.264 decoder\n");
        md_stream_destroy(stream);
        cleanup_session_nostr(&nostr, &ctx);
        md_signer_destroy(signer);
        md_account_startup_clear(&account);
        return 1;
    }

    /* Create renderer (SDL2 window) and overlay */
    if (do_display) {
        ctx.renderer = md_renderer_create(1920, 1080, "metadesk");
        if (!ctx.renderer) {
            fprintf(stderr, "ERROR: cannot open the GUI window; "
                            "use --no-display for decode-only\n");
            md_decoder_destroy(decoder);
            md_stream_destroy(stream);
            cleanup_session_nostr(&nostr, &ctx);
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
            md_renderer_close_display();
            return 1;
        } else {
            /* Create Dear ImGui overlay on top of the renderer */
            ctx.overlay = md_overlay_create(
                md_renderer_get_sdl_window(ctx.renderer),
                md_renderer_get_sdl_renderer(ctx.renderer));
            if (!ctx.overlay) {
                fprintf(stderr, "WARNING: ImGui overlay unavailable\n");
            } else {
                /* Single event pump: renderer forwards every event here */
                md_renderer_set_event_callback(ctx.renderer,
                                               overlay_event_forward,
                                               &ctx);
            }
        }
    }

    stream = connect_with_ui(host, npub, fips_control_socket,
                                   port, timeout_ms, nostr,
                                   host_pubkey_hex, &ctx, &session);
    if (!stream && !ctx.renderer) {
        md_decoder_destroy(decoder);
        cleanup_session_nostr(&nostr, &ctx);
        md_signer_destroy(signer);
        md_account_startup_clear(&account);
        if (!g_running) {
            printf("client: done. decoded 0 frames, displayed 0, tree updates 0\n");
            return 0;
        }
        return 1;
    }
    ctx.paused = !stream;
    if (stream) printf("client: connected\n");

    /* ── Main loop ───────────────────────────────────────────── */
    printf("client: receiving stream... (Ctrl+C to stop)\n\n");

    uint32_t pkt_seq = 0;
    uint32_t last_stats_ms = md_stream_now_ms();
    uint32_t video_packets = 0;
    uint32_t reconnect_delay_ms = 1000;
    const uint32_t reconnect_delay_max_ms = 30000;
    bool deliberate_exit = false;

    while (g_running) {
        if (ctx.paused) {
            if (stream) { md_stream_destroy(stream); stream = NULL; }
            reset_client_video(&ctx);
            md_session_reset(&session);
            ctx.phase = MD_CLIENT_IDLE;
            ctx.reconnecting = 0;
            ctx.reconnect_delay_ms = 0;
            pump_client_ui(&ctx, NULL);
            /* UI cadence only: input is drained while disconnected. */
            SDL_Delay(16);
            if (!ctx.paused) ctx.cancel_connect = false;
            continue;
        }
        if (!stream || !md_stream_is_connected(stream)) {
            if (stream) {
                ctx.stream = NULL;
                md_stream_destroy(stream);
                stream = NULL;
            }
            md_session_reset(&session);
            reset_client_video(&ctx);
            md_decoder_destroy(decoder);
            decoder = md_decoder_create();
            if (!decoder) break;

            ctx.reconnecting = 1;
            if (ctx.session_accepted >= 0)
                ctx.connection_status = "Disconnected — reconnecting...";
            ctx.reconnect_delay_ms = reconnect_delay_ms;
            render_client_overlay(&ctx, NULL);

            fprintf(stderr, "client: reconnecting in %.1fs\n",
                    (double)reconnect_delay_ms / 1000.0);
            if (sleep_with_reconnect_ui(&ctx, reconnect_delay_ms,
                                        &deliberate_exit) != 0 ||
                deliberate_exit || !g_running) {
                if (ctx.paused && g_running) continue;
                break;
            }

            stream = connect_with_ui(host, npub, fips_control_socket,
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

        /* Keep the input-forwarding path pointed at the live stream. */
        ctx.stream = stream;

        pump_client_ui(&ctx, stream);
        if (!g_running || ctx.paused) continue;

        /* Receive next packet */
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        int ret = md_stream_recv(stream, &hdr, &payload, 16 /* ~1 frame at 60fps */);

        if (ret == 1) {
            /* Timeout — no data yet, keep UI responsive and loop back. */
            render_client_overlay(&ctx, stream);
            continue;
        }
        if (ret < 0) {
            /* Unexpected connection loss — enter reconnect loop. */
                fprintf(stderr, "client: connection lost\n");
            ctx.stream = NULL;
            md_stream_destroy(stream);
            stream = NULL;
            md_session_reset(&session);
            /* The next stream's frames are numbered afresh: map no point
             * until one of them is shown. */
            ctx.uploaded = ctx.shown = (FrameTag){ 0 };
            ctx.reconnecting = 1;
            ctx.connection_status = "Disconnected — reconnecting...";
            ctx.reconnect_delay_ms = reconnect_delay_ms;
            continue;
        }

        /* Handle packet by type */
        switch (hdr.type) {
        case MD_PKT_VIDEO_FRAME: {
            video_packets++;

            /* Forward input queued while the current frame is on screen
             * before this one is decoded, uploaded (which may resize the
             * window) and presented: it is mapped and tagged against the
             * frame the user saw (md-383x). */
            if (ctx.renderer && md_renderer_poll_events(ctx.renderer) < 0) {
                printf("client: window closed\n");
                deliberate_exit = true;
                g_running = 0;
                break;
            }

            int64_t dec_start = now_us();

            ret = md_decoder_submit(decoder, payload, hdr.payload_len, hdr.sequence);
            if (ret == 0) {
                md_decoder_poll(decoder, on_decoded, &ctx);
            }

            int64_t dec_elapsed = now_us() - dec_start;
            ctx.total_decode_us += dec_elapsed;

            /* Composite the freshly decoded frame + overlay and present now,
             * rather than waiting for the next recv timeout. */
            render_client_overlay(&ctx, stream);
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
                        ctx.tree_updates++;
                        if (ctx.tree_updates <= 3 || (ctx.tree_updates % 100) == 0) {
                            fprintf(stderr, "client: applied UI tree delta (%u bytes, update #%u)\n",
                                    hdr.payload_len, ctx.tree_updates);
                        }
                    } else {
                        ctx.tree_patch_failures++;
                        if (ctx.tree_patch_failures <= 3 || (ctx.tree_patch_failures % 100) == 0) {
                            fprintf(stderr, "client: warning: UI tree delta patch failed\n");
                        }
                    }
                }
            }
            break;

        case MD_PKT_SESSION_STATE: {
            if (hdr.payload_len > MD_SESSION_JSON_MAX || !hdr.payload_len ||
                memchr(payload, 0, hdr.payload_len)) { md_stream_close(stream); break; }
            char json[MD_SESSION_JSON_MAX + 1];
            memcpy(json, payload, hdr.payload_len); json[hdr.payload_len] = 0;
            cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
            cJSON *type=cJSON_GetObjectItemCaseSensitive(root,"type");
            if (cJSON_IsString(type) && !strcmp(type->valuestring,"outputs")) {
                bool present;
                if (md_session_outputs_read(root,&ctx.outputs,&present) || !present) md_stream_close(stream);
                else {
                    cJSON *error=cJSON_GetObjectItemCaseSensitive(root,"error");
                    fprintf(stderr,"client: output selected %s%s%s\n",ctx.outputs.selected,
                        cJSON_IsString(error) ? ": " : "",cJSON_IsString(error) ? error->valuestring : "");
                    if (!cJSON_IsString(error)) strcpy(ctx.requested_output,ctx.outputs.selected);
                }
                cJSON_Delete(root); break;
            }
            bool present; MdQuality applied;
            if (!root || md_quality_read(root, &applied, &present) || !present) md_stream_close(stream);
            else {
                ctx.effective_quality = applied;
                fprintf(stderr, "client: quality applied cap=%ux%u bitrate=%u fps=%u\n",
                    applied.max_width, applied.max_height, applied.bitrate, applied.fps);
            }
            cJSON_Delete(root); break;
        }
        case MD_PKT_SESSION_INFO:
            fprintf(stderr, "client: received session info\n");
            break;

        default:
            fprintf(stderr, "client: unknown packet type 0x%02x\n", hdr.type);
            break;
        }

        free(payload);

        /* Update and render overlay */
        render_client_overlay(&ctx, stream);

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

    cleanup_session_nostr(&nostr, &ctx);
    md_signer_destroy(signer);
    md_account_startup_clear(&account);
    free(ctx.ui_tree_json);
    if (ctx.overlay)
        md_overlay_destroy(ctx.overlay);
    if (ctx.renderer)
        md_renderer_destroy(ctx.renderer);
    md_renderer_close_display();

    printf("client: done. decoded %u frames, displayed %u, tree updates %u\n",
           ctx.frames_decoded, ctx.frames_displayed, ctx.tree_updates);
    return 0;
}

int main(int argc, char **argv) {
    int status = client_run(argc, argv);
    /* Drain the process-wide network runtime before ExitProcess kills its
     * threads. DLL atexit handlers run too late to join them on Windows. */
    md_nostr_shutdown();
    return status;
}
