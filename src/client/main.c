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
#include "signer.h"
#include "nostr.h"
#include "a11y.h"
#include "ui.h"
#include "packet.h"
#include "input_event.h"
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

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
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
    char          stream_token[65];       /* ephemeral DM-only proof    */
    uint32_t      granted_caps;       /* from session_accept DM          */
    MdTreeFormat  accepted_tree_format; /* confirmed tree format          */
    volatile int  session_accepted;   /* set by on_dm callback           */
    GoChannel      *session_ch;       /* signaled on session accepted    */
    MdAccountWatch *owner_watch;      /* also guards direct-IP sessions  */
    MdAccountBinding *owner;          /* borrowed from startup until drain */

    /* Connection recovery UI state */
    int            reconnecting;      /* unexpected disconnect recovery  */
    uint32_t       reconnect_delay_ms; /* next retry delay for overlay     */
    const char    *connection_status; /* transient user-facing detail     */
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
    return g_running && (!ctx->owner || md_account_binding_is_current(ctx->owner));
}

static void client_owner_revoked(void *data) {
    ClientCtx *ctx = data;
    g_running = 0;
    if (ctx->session_ch) go_channel_close(ctx->session_ch);
}

/* Guards pending_req/request_pending and the accepted_* fields between the
 * DM worker and the negotiating thread. */
static pthread_mutex_t g_negotiation_mu = PTHREAD_MUTEX_INITIALIZER;

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
    if (matched) {
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
    ctx->accepted_session_id[0] = '\0';
    OPENSSL_cleanse(ctx->stream_token, sizeof(ctx->stream_token));
    ctx->granted_caps = 0;
    ctx->accepted_tree_format = MD_TREE_FORMAT_COMPACT;
    ctx->session_accepted = 0;
}

static int negotiate_session(MdNostr *nostr, ClientCtx *ctx,
                                  const char *host_pubkey_hex,
                                  uint32_t timeout_ms,
                                  MdSessionRequest *out_req) {
    if (!nostr || !ctx || !host_pubkey_hex || !out_req || !client_owner_current(ctx))
        return -1;

    MdSessionRequest req = {
        .capabilities = MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT,
        .tree_format = MD_TREE_FORMAT_COMPACT,
    };
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
        go_select_timeout(cases, 1, (uint64_t)(left_us + 999) / 1000);
    }
    pthread_mutex_lock(&g_negotiation_mu);
    ctx->request_pending = 0; /* late answers to this request are ignored */
    accepted = ctx->session_accepted;
    pthread_mutex_unlock(&g_negotiation_mu);

    if (!client_owner_current(ctx)) {
        fprintf(stderr, "ERROR: client role revoked during session negotiation\n");
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
    ret = md_stream_recv(stream, &hdr, &payload, MD_SESSION_BIND_TIMEOUT_MS);
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
    return valid && client_owner_current(ctx) ? 0 : -1;
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

    if (!host) {
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

    }

    /* Endpoint selection never bypasses identity-pinned Nostr admission. */
    MdSessionRequest req;
    if (negotiate_session(nostr, ctx, host_pubkey_hex,
                          timeout_ms, &req) != 0 || !client_owner_current(ctx))
        return NULL;

    if (host) {
        printf("client: connecting to %s:%u...\n", host, port);
        stream = md_stream_connect(host, port, timeout_ms);
    } else {
        printf("client: connecting via FIPS to %.*s...\n", 12, npub);
        stream = md_stream_connect_fips(npub, port, timeout_ms);
    }
    if (!stream) {
        fprintf(stderr, "ERROR: %s connect failed\n", host ? "direct TCP" : "FIPS");
        return NULL;
    }
    if (!client_owner_current(ctx) || send_session_info(stream, ctx) != 0 ||
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
    if (!ctx->stream) return;
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
    md_overlay_handle_sdl_event(ctx->overlay, event);
    forward_input_event(ctx, (const SDL_Event *)event);
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

    /* One composite per displayed frame: draw the latest video texture,
     * then the overlay on top, then a single present. Drawing both into the
     * same back buffer is what stops the overlay leaving ghost trails. */
    md_renderer_begin_frame(ctx->renderer);
    md_overlay_new_frame(ctx->overlay);
    md_overlay_render(ctx->overlay, &overlay_stats);
    md_renderer_end_frame(ctx->renderer);
    ctx->shown = ctx->uploaded;
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
    /* Progress lines must reach a pipe (container log, supervisor) when
     * printed, not when a block buffer fills or the process exits. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
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
    ClientCtx ctx = { .owner = account.binding };
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

    if (!g_running) {
        cleanup_session_nostr(&nostr, &ctx);
        md_signer_destroy(signer);
        md_account_startup_clear(&account);
        return 1;
    }
    stream = connect_client_stream(host, npub, fips_control_socket,
                                   port, timeout_ms, nostr,
                                   host_pubkey_hex, &ctx, &session);
    if (!stream) {
        cleanup_session_nostr(&nostr, &ctx);
        md_signer_destroy(signer);
        md_account_startup_clear(&account);
        return 1;
    }
    printf("client: connected\n");

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
                ctx.stream = NULL;
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

        /* Keep the input-forwarding path pointed at the live stream. */
        ctx.stream = stream;

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
            render_client_overlay(&ctx, stream, last_stats_ms);
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
