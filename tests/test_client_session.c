/* Real client DM gate and negotiation wait; no renderer, signer or desktop. */
#define main md_client_main_not_run
#define md_stream_connect_fips test_fips_dial
#define md_nostr_send_session_request test_request_publish
#define md_nostr_get_npub test_client_pubkey
#include "../src/client/main.c"
#undef main
#include "md_test.h"

static const char *host_pk = "host-pinned-by-client";
static ClientCtx *publishing_ctx;
static const char *refusal_reason;
static GoChannel *request_sent;

const char *test_client_pubkey(const MdNostr *nostr) {
    (void)nostr;
    return "fixture-client";
}
int test_request_publish(MdNostr *nostr, const char *peer, const char *json) {
    (void)nostr;
    MD_CHECK(!strcmp(peer, host_pk));
    MdSessionRequest req;
    MD_CHECK(md_session_request_from_json(json, &req) == 0);
    if (request_sent) {
        go_channel_try_send(request_sent, (void *)(uintptr_t)1);
        return 0;
    }
    MdSessionReject reject = {0};
    memcpy(reject.request_id, req.request_id, sizeof(reject.request_id));
    snprintf(reject.reason, sizeof(reject.reason), "%s", refusal_reason);
    char *answer = md_session_reject_to_json(&reject);
    MD_CHECK(answer);
    MdNostrDmInfo info = {.rumor_created_at = req.issued_at, .live = true};
    on_session_dm("wrong-host", answer, &info, publishing_ctx);
    MD_CHECK(!publishing_ctx->session_accepted && publishing_ctx->request_pending);
    MdNostrDmInfo stale = {.rumor_created_at = req.issued_at - 3600, .live = true};
    on_session_dm(host_pk, answer, &stale, publishing_ctx);
    MD_CHECK(!publishing_ctx->session_accepted && !publishing_ctx->connection_status);
    char saved = reject.request_id[0];
    reject.request_id[0] = saved == 'a' ? 'b' : 'a';
    char *other = md_session_reject_to_json(&reject);
    on_session_dm(host_pk, other, &info, publishing_ctx);
    MD_CHECK(!publishing_ctx->session_accepted && !publishing_ctx->connection_status);
    free(other);
    on_session_dm(host_pk, answer, &info, publishing_ctx);
    MD_CHECK(publishing_ctx->session_accepted == -1 && !publishing_ctx->request_pending);
    const char *status = publishing_ctx->connection_status;
    on_session_dm(host_pk, answer, &info, publishing_ctx); /* duplicate is inert */
    MD_CHECK(publishing_ctx->connection_status == status);
    free(answer);
    return 0;
}

#ifndef _WIN32
static void *terminate_pending(void *unused) {
    (void)unused;
    void *value;
    go_channel_receive(request_sent, &value);
    kill(getpid(), SIGTERM);
    return NULL;
}
static void test_pending_shutdown(void) {
    MD_CHECK(start_signal_wake() == 0);
    request_sent = go_channel_create(1);
    ClientCtx ctx = { .session_ch = go_channel_create(1) };
    pthread_t terminator;
    MD_CHECK(pthread_create(&terminator, NULL, terminate_pending, NULL) == 0);
    MdSessionRequest out;
    int64_t start = now_us();
    MD_CHECK(negotiate_session((MdNostr *)(uintptr_t)1, &ctx, host_pk, 180000, &out) < 0);
    MD_CHECK(now_us() - start < 5000000); /* failure-only shutdown bound */
    MD_CHECK(!g_running && !ctx.request_pending && !ctx.session_accepted);
    pthread_join(terminator, NULL);
    stop_signal_wake();
    go_channel_unref(request_sent);
    request_sent = NULL;
    go_channel_unref(ctx.session_ch);
    g_running = 1;
}
#endif

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
static unsigned dial_calls;
static bool dial_succeeds;
MdStream *test_fips_dial(const char *npub, uint16_t port, uint32_t timeout) {
    MD_CHECK(!strcmp(npub, "npub1peer") && port == 1234);
    MD_CHECK(timeout > 0 && timeout <= 1000);
    dial_calls++;
    return dial_succeeds && dial_calls == 2 ? (MdStream *)(uintptr_t)1 : NULL;
}

typedef struct { int fd; unsigned count; } RetryControl;
static void *retry_control(void *data) {
    RetryControl *srv = data;
    for (unsigned i = 0; i < srv->count; i++) {
        struct pollfd pfd = {.fd = srv->fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 5000);
        MD_CHECK(ready > 0);
        if (ready <= 0) return NULL;
        int fd = accept(srv->fd, NULL, NULL);
        MD_CHECK(fd >= 0);
        if (fd < 0) return NULL;
        char request[1024] = {0};
        size_t n = 0;
        while (n + 1 < sizeof(request)) {
            ssize_t received = read(fd, request + n, 1);
            MD_CHECK(received == 1);
            if (received != 1) { close(fd); return NULL; }
            if (request[n++] == '\n') break;
        }
        MD_CHECK(strstr(request, "show_peers"));
        /* Link disappears between dial attempts, then returns. */
        const char *reply = i == 1
            ? "{\"status\":\"ok\",\"data\":{\"peers\":[{\"npub\":\"npub1peer\",\"connectivity\":\"disconnected\"}]}}\n"
            : "{\"status\":\"ok\",\"data\":{\"peers\":[{\"npub\":\"npub1peer\",\"connectivity\":\"connected\",\"link_id\":7}]}}\n";
        MD_CHECK(write(fd, reply, strlen(reply)) == (ssize_t)strlen(reply));
        close(fd);
    }
    return NULL;
}
static void test_fips_retry(void) {
    char path[108];
    snprintf(path, sizeof(path), "/tmp/md-w6-retry-%d.sock", getpid());
    RetryControl srv = {.fd = socket(AF_UNIX, SOCK_STREAM, 0), .count = 3};
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    MD_CHECK(bind(srv.fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    MD_CHECK(listen(srv.fd, 3) == 0);
    pthread_t thread;
    MD_CHECK(pthread_create(&thread, NULL, retry_control, &srv) == 0);
    ClientCtx ctx = {.session_ch = go_channel_create(1), .request_started_us = now_us()};
    dial_calls = 0; dial_succeeds = true;
    MD_CHECK(dial_fips_session("npub1peer", path, 1234, 5000, &ctx) == (MdStream *)(uintptr_t)1);
    MD_CHECK(dial_calls == 2); /* no renegotiation or dial without a link */
    pthread_join(thread, NULL);
    ctx.request_started_us = now_us() - (int64_t)MD_SESSION_TOKEN_LIFETIME_MS * 1000;
    MD_CHECK(!dial_fips_session("npub1peer", path, 1234, 5000, &ctx));
    MD_CHECK(dial_calls == 2); /* expired proof never dials */
    srv.count = 1; dial_calls = 0; dial_succeeds = false;
    MD_CHECK(pthread_create(&thread, NULL, retry_control, &srv) == 0);
    ctx.request_started_us = now_us() - (int64_t)MD_SESSION_TOKEN_LIFETIME_MS * 1000 + 80000;
    MD_CHECK(!dial_fips_session("npub1peer", path, 1234, 5000, &ctx));
    MD_CHECK(dial_calls == 1); /* backoff clipped to token deadline */
    pthread_join(thread, NULL);
    go_channel_unref(ctx.session_ch);
    close(srv.fd); unlink(path);
}
#else
MdStream *test_fips_dial(const char *npub, uint16_t port, uint32_t timeout) {
    (void)npub; (void)port; (void)timeout; return NULL;
}
#endif

typedef struct { MdStream *peer; ClientCtx *ctx; } BindCancel;
static void *cancel_after_bind(void *data) {
    BindCancel *job = data;
    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    MD_CHECK(md_stream_recv(job->peer, &hdr, &payload, 2000) == 0);
    MD_CHECK(hdr.type == MD_PKT_SESSION_INFO);
    free(payload);
    cancel_client_connect(job->ctx);
    return NULL;
}
static void test_bind_cancel(void) {
    for (int after_proof = 0; after_proof < 2; after_proof++) {
        MdStreamServer *server = md_stream_server_create("::1", 0);
        MD_CHECK(server);
        MdStream *client = md_stream_connect("::1", md_stream_server_port(server), 1000);
        MD_CHECK(client);
        MdStream *peer = md_stream_server_accept(server, 1000);
        MD_CHECK(peer);
        md_stream_server_destroy(server);
        ClientCtx ctx = {.cancel_ch = go_channel_create(1),
                         .request_started_us = now_us(), .granted_caps = MD_CAP_VIDEO};
        snprintf(ctx.accepted_session_id, sizeof(ctx.accepted_session_id), "w6-binding");
        memset(ctx.stream_token, 'a', 64);
        BindCancel job = {peer, &ctx};
        pthread_t thread;
        if (after_proof) MD_CHECK(pthread_create(&thread, NULL, cancel_after_bind, &job) == 0);
        else cancel_client_connect(&ctx);
        int64_t start = now_us();
        MD_CHECK(bind_client_stream(client, &ctx) < 0);
        MD_CHECK(now_us() - start < 2000000); /* cancellation, not five-second ACK timeout */
        if (after_proof) pthread_join(thread, NULL);
        else {
            MdPacketHeader hdr;
            uint8_t *payload = NULL;
            MD_CHECK(md_stream_recv(peer, &hdr, &payload, 1000) < 0); /* no proof after cancellation */
            free(payload);
        }
        MD_CHECK(!md_stream_is_connected(client));
        MD_CHECK(ctx.cancel_connect && !ctx.binding_stream);
        md_stream_destroy(client); md_stream_destroy(peer);
        go_channel_unref(ctx.cancel_ch);
    }
}

static void test_overlay_controls(void) {
    /* Real SDL + ImGui, isolated software window, no host input backend. */
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    MD_CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
    SDL_Window *window = SDL_CreateWindow("W6 overlay test", 0, 0, 640, 480, SDL_WINDOW_HIDDEN);
    MD_CHECK(window);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    MD_CHECK(renderer);
    MdOverlay *overlay = md_overlay_create(window, renderer);
    MD_CHECK(overlay);
    SDL_Event key = {.type = SDL_KEYDOWN};
    key.key.keysym.sym = SDLK_F1;
    key.key.keysym.scancode = SDL_SCANCODE_F1;
    MD_CHECK(md_overlay_handle_sdl_event(overlay, &key));
    key.key.repeat = 1;
    MD_CHECK(md_overlay_handle_sdl_event(overlay, &key));
    key.key.repeat = 0; key.key.keysym.sym = SDLK_ESCAPE;
    key.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
    MD_CHECK(md_overlay_handle_sdl_event(overlay, &key));
    key.type = SDL_KEYUP;
    MD_CHECK(md_overlay_handle_sdl_event(overlay, &key));
    key.key.keysym.sym = SDLK_a;
    key.key.keysym.scancode = SDL_SCANCODE_A;
    MD_CHECK(!md_overlay_handle_sdl_event(overlay, &key));
    MD_CHECK(md_overlay_take_action(overlay) == MD_OVERLAY_NONE);
    for (unsigned phase = MD_CLIENT_IDLE; phase <= MD_CLIENT_BACKOFF; phase++) {
        MdOverlayStats stats = {.phase = phase, .error = MD_CLIENT_ERROR_TRANSPORT,
            .connected = phase == MD_CLIENT_STREAMING,
            .granted_caps = MD_CAP_VIDEO | MD_CAP_INPUT};
        md_overlay_new_frame(overlay);
        md_overlay_render(overlay, &stats);
        MD_CHECK(md_client_phase_label(phase)[0]);
    }
    MD_CHECK(strstr(md_client_error_label(MD_CLIENT_ERROR_ADMISSION), "authorization"));
    ClientCtx ctx = {.stream = (MdStream *)(uintptr_t)1,
        .shown = {640, 480, 99}, .uploaded = {640, 480, 99}, .input_seq = 12,
        .ui_tree_json = strdup("old tree"), .ui_tree_len = 8};
    reset_client_video(&ctx);
    MD_CHECK(!ctx.stream && !ctx.shown.w && !ctx.uploaded.w && !ctx.input_seq);
    MD_CHECK(!ctx.ui_tree_json && !ctx.ui_tree_len);
    /* Pending UI cancellation wakes the exact same select as shutdown. */
    ctx.cancel_ch = go_channel_create(1);
    ctx.cancel_connect = true;
    go_channel_close(ctx.cancel_ch);
    MD_CHECK(wait_client_backoff(&ctx, 30000) < 0);
    go_channel_unref(ctx.cancel_ch);
    md_overlay_destroy(overlay);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
}

/* Ordered TCP sentinel proves absence without a timeout or desktop input. */
static void test_input_grants(void) {
    const uint32_t grants[] = {0, MD_CAP_VIDEO, MD_CAP_VIDEO | MD_CAP_AGENT,
                              MD_CAP_VIDEO | MD_CAP_INPUT};
    MdStreamServer *server = md_stream_server_create("::1", 0);
    MD_CHECK(server != NULL);
    if (!server) return;
    MdStream *sender = md_stream_connect("::1", md_stream_server_port(server), 1000);
    MdStream *peer = md_stream_server_accept(server, 1000);
    MD_CHECK(sender && peer);
    if (!sender || !peer) goto done;
    for (size_t i = 0; i < sizeof(grants) / sizeof(grants[0]); i++) {
        ClientCtx ctx = {.stream = sender, .granted_caps = grants[i],
                         .shown = {.w = 640, .h = 480, .pkt_seq = 7}};
        send_input(&ctx, MD_INPUT_EV_MOVE, 0, 20, 30, NULL, 0);
        send_input(&ctx, MD_INPUT_EV_BUTTON, 1, 20, 30, NULL, 0);
        send_input(&ctx, MD_INPUT_EV_WHEEL, 0, 0, 1, NULL, 0);
        send_input(&ctx, MD_INPUT_EV_KEY, 0, 0, 0, "a", 1);
        unsigned expected = (grants[i] & MD_CAP_INPUT) ? 4 : 0;
        MD_CHECK(ctx.input_seq == expected);
        MD_CHECK(md_stream_send(sender, MD_PKT_PING, 99, NULL, 0) == 0);
        unsigned inputs = 0;
        for (;;) {
            MdPacketHeader hdr;
            uint8_t *payload = NULL;
            int rc = md_stream_recv(peer, &hdr, &payload, 1000);
            MD_CHECK(rc == 0);
            if (rc != 0) { free(payload); break; }
            if (hdr.type == MD_PKT_PING) {
                MD_CHECK(hdr.sequence == 99 && hdr.payload_len == 0);
                free(payload);
                break;
            }
            MD_CHECK(hdr.type == MD_PKT_INPUT && hdr.payload_len > 0);
            inputs++;
            free(payload);
        }
        MD_CHECK(inputs == expected);
        MD_CHECK(md_stream_is_connected(sender) && md_stream_is_connected(peer));
    }
done:
    md_stream_destroy(sender);
    md_stream_destroy(peer);
    md_stream_server_destroy(server);
}

static void test_tls_acceptance_gate(void) {
    for (unsigned supported = 0; supported < 2; supported++) {
        ClientCtx ctx = {0};
        snprintf(ctx.expected_host_pk, sizeof(ctx.expected_host_pk), "%s", host_pk);
        MD_CHECK(md_session_request_prepare(&ctx.pending_req, time(NULL), 30) == 0);
        ctx.pending_req.capabilities = MD_CAP_VIDEO;
        ctx.request_pending = 1;
        MdSessionAccept acc = {.session_id = "tls-session", .granted = MD_CAP_VIDEO};
        memcpy(acc.request_id, ctx.pending_req.request_id, sizeof(acc.request_id));
        memset(acc.stream_token, 'a', 64);
        if (supported) memset(acc.tls_cert_sha256, 'b', 64);
        char *json = md_session_accept_to_json(&acc);
        MdNostrDmInfo info = {.live = true, .rumor_created_at = ctx.pending_req.issued_at};
        on_session_dm(host_pk, json, &info, &ctx);
        MD_CHECK(ctx.session_accepted == (supported ? 1 : -1));
        MD_CHECK(!ctx.request_pending);
        if (supported) MD_CHECK(!strcmp(ctx.tls_cert_sha256, acc.tls_cert_sha256));
        else MD_CHECK(!ctx.stream_token[0] && strstr(ctx.connection_status, "TLS 1.3"));
        free(json);
        reset_session_accept_state(&ctx);
        MD_CHECK(!ctx.tls_cert_sha256[0] && !ctx.stream_token[0]);
    }
}

int main(void) {
    test_tls_acceptance_gate();
    test_input_grants();
    const char *reasons[] = {"capture_unavailable", "request_refused", "tls_required"};
    for (unsigned i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        ClientCtx ctx = {0};
        snprintf(ctx.expected_host_pk, sizeof(ctx.expected_host_pk), "%s", host_pk);
        ctx.session_ch = go_channel_create(1);
        publishing_ctx = &ctx;
        refusal_reason = reasons[i];
        MdSessionRequest out = {0};
        MD_CHECK(negotiate_session((MdNostr *)(uintptr_t)1, &ctx, host_pk, 10000, &out) < 0);
        MD_CHECK(ctx.session_accepted == -1 && !ctx.stream_token[0] && !ctx.granted_caps);
        MD_CHECK(ctx.connection_status && strstr(ctx.connection_status, "host"));
        if (!i) MD_CHECK(strstr(ctx.connection_status, "authorize screen sharing"));
        pthread_mutex_lock(&g_negotiation_mu);
        reset_session_accept_state(&ctx);
        pthread_mutex_unlock(&g_negotiation_mu);
        MD_CHECK(!ctx.connection_status && !ctx.session_accepted);
        go_channel_close(ctx.session_ch);
        go_channel_unref(ctx.session_ch);
    }
#ifndef _WIN32
    test_pending_shutdown();
    test_fips_retry();
#endif
    test_bind_cancel();
    test_overlay_controls();
    return md_test_report();
}
