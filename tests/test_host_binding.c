#include "md_socket.h"
/* Private loopback tests of real host admission/capture/dispatch. No desktop. */
#define main md_host_main_not_run
#define md_agent_tree_start test_tree_start
#define md_agent_handle_action test_action
#define md_input_mouse_move test_move
#define md_nostr_send_session_accept test_publish
#define md_agent_create test_create_agent
#define md_capture_create test_capture_create
#include "../src/host/main.c"
#undef main
#undef md_nostr_send_session_accept
#undef md_agent_create
#undef md_capture_create
#include "md_test.h"
#include "decode.h"
/* nostr.h's prototype was renamed by the macro above; restore the real one. */
int md_nostr_send_session_accept(MdNostr *n, const char *client_pubkey_hex,
                                 const char *json_payload);

MdAgent *md_agent_create(const MdAgentConfig *cfg);
static bool fake_output_capture, fail_output_capture;
MdCaptureCtx *md_capture_create(const MdCaptureConfig *cfg);
static int output_start(MdCaptureCtx *c) { c->active=true; return 0; }
static int output_frame(MdCaptureCtx *c,MdFrame *f,uint32_t timeout) {
    (void)timeout; static uint8_t pixels[64*64*4]; memset(pixels,123,sizeof(pixels));
    c->width=c->height=64;
    c->output_rect=(MdCaptureRect){!strcmp(c->config.output,"id:2") ? 640 : 0,0,640,360};
    *f=(MdFrame){.width=64,.height=64,.stride=256,.data=pixels,.data_size=sizeof(pixels),
        .buf_type=MD_BUF_CPU,.format=MD_PIX_CAPTURE_BGRA}; return 0;
}
static void output_release(MdCaptureCtx *c,MdFrame *f) { (void)c; (void)f; }
MdCaptureCtx *test_capture_create(const MdCaptureConfig *cfg) {
    if (!fake_output_capture) return md_capture_create(cfg);
    if (fail_output_capture) return NULL;
    static const MdCaptureBackend b={.start=output_start,.get_frame=output_frame,.release_frame=output_release};
    MdCaptureCtx *c=calloc(1,sizeof(*c)); MD_CHECK(c); c->vtable=&b; c->config=*cfg; return c;
}
static bool fail_agent_create;
MdAgent *test_create_agent(const MdAgentConfig *cfg) {
    return fail_agent_create ? NULL : md_agent_create(cfg);
}

static unsigned tree_calls, action_calls, input_calls, publish_calls;
static MdSessionAccept published;
static MdSessionReject rejected;
static unsigned reject_calls;
/* Records the acceptance; with a real bridge (relay fixture) also publishes. */
int test_publish(MdNostr *n, const char *peer, const char *json) {
    if (md_session_reject_from_json(json, &rejected) == 0) {
        reject_calls++;
    } else {
        publish_calls++;
        MD_CHECK(md_session_accept_from_json(json, &published) == 0);
    }
    return n ? md_nostr_send_session_accept(n, peer, json) : 0;
}
int md_host_account_cli(int argc, char **argv) { (void)argc; (void)argv; return -1; }
int md_host_access_cli(int argc, char **argv) { (void)argc; (void)argv; return -1; }
MdAgentTreeJob *test_tree_start(MdAgent *a) {
    (void)a; tree_calls++; return NULL;
}
int test_action(MdAgent *a, MdStream *s, uint32_t *seq,
                const uint8_t *p, uint32_t n) {
    (void)a; (void)s; (void)seq; (void)p; (void)n; action_calls++; return 0;
}
static int move_x, move_y;
int test_move(MdInput *i, int x, int y) {
    (void)i; move_x = x; move_y = y; input_calls++; return 0;
}

typedef struct { MdStream *peer, *host; } Pair;
static Pair pair(void) {
    MdStreamServer *srv = md_stream_server_create("::1", 0);
    MD_CHECK(srv);
    Pair p = { .peer = md_stream_connect("::1", md_stream_server_port(srv), 1000) };
    MD_CHECK(p.peer);
    p.host = md_stream_server_accept(srv, 1000);
    MD_CHECK(p.host);
    md_stream_server_destroy(srv);
    return p;
}
static void destroy_pair(Pair p) {
    md_stream_destroy(p.peer); md_stream_destroy(p.host);
}
static MdSessionAccept reserve(MdSession *s, uint32_t caps) {
    md_session_init(s);
    MD_CHECK(md_session_request(s, "authenticated-client", caps, MD_TREE_FORMAT_COMPACT) == 0);
    MD_CHECK(md_session_accept(s, "negotiated-id", caps) == 0);
    MdSessionAccept acc;
    MD_CHECK(md_session_prepare_binding(s, &acc, (uint64_t)md_now_ms()) == 0);
    char *dm = md_session_accept_to_json(&acc);
    MD_CHECK(dm && md_session_accept_from_json(dm, &acc) == 0);
    free(dm);
    return acc;
}
static void send_proof(Pair p, const MdSessionAccept *acc) {
    char *json = md_session_bind_to_json(acc);
    MD_CHECK(json && strlen(json) <= MD_SESSION_JSON_MAX);
    MD_CHECK(md_stream_send(p.peer, MD_PKT_SESSION_INFO, 0,
                            (const uint8_t *)json, (uint32_t)strlen(json)) == 0);
    free(json);
}
static void ack(Pair p, uint32_t caps) {
    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    MD_CHECK(md_stream_recv(p.peer, &hdr, &payload, 1000) == 0);
    MD_CHECK(hdr.type == MD_PKT_SESSION_INFO && hdr.payload_len < MD_SESSION_JSON_MAX);
    char json[MD_SESSION_JSON_MAX + 1];
    memcpy(json, payload, hdr.payload_len); json[hdr.payload_len] = '\0';
    MdSessionAccept acc;
    MD_CHECK(md_session_accept_from_json(json, &acc) == 0);
    MD_CHECK(acc.granted == caps && !acc.stream_token[0]);
    MD_CHECK(strcmp(acc.session_id, "negotiated-id") == 0);
    free(payload);
}
static void closed(Pair p) {
    MD_CHECK(!md_stream_is_connected(p.host));
    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    MD_CHECK(md_stream_recv(p.peer, &hdr, &payload, 1000) < 0);
    free(payload);
}
/* The frame the client tags its input with (md-383x): TEST_FRAME_W x
 * TEST_FRAME_H from packet 0 on; one packet has gone out. */
#define TEST_FRAME_W 640
#define TEST_FRAME_H 360
static const uint32_t test_pkt_seq = 1;
static HostCtx context(MdSession *s) {
    HostCtx ctx = { .pending_req = {.stream_tls = true}, .tls_cert_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", .session = s, .agent = (MdAgent *)(uintptr_t)1,
                    .input = (MdInput *)(uintptr_t)1 };
    host_capture_sync_init(&ctx.sync, NULL, NULL, NULL, &test_pkt_seq,
                           TEST_FRAME_W, TEST_FRAME_H, NULL);
    host_capture_sync_packet_sent(&ctx.sync,
                                  host_capture_sync_frame_tag(&ctx.sync), 0);
    return ctx;
}
/* Dispatch one MOVE to (x, y) on the frame_w x frame_h frame of video
 * packet seq; legacy omits the frame tag (a client built before md-383x).
 * True if it injected. */
static bool dispatch_move(HostCtx *ctx, int16_t x, int16_t y, uint16_t frame_w,
                          uint16_t frame_h, uint32_t seq, bool legacy) {
    uint8_t buf[MD_INPUT_EVENT_HDR_LEN + MD_INPUT_EVENT_TAG_LEN];
    MD_CHECK(md_input_event_pack(buf, sizeof(buf), MD_INPUT_EV_MOVE, 0, x, y,
                                 NULL, 0, frame_w, frame_h, seq) == sizeof(buf));
    MdPacketHeader hdr = { .type = MD_PKT_INPUT,
                           .payload_len = legacy ? MD_INPUT_EVENT_HDR_LEN
                                                 : sizeof(buf) };
    unsigned before = input_calls;
    handle_client_packet(ctx, &hdr, buf);
    return input_calls == before + 1;
}
/* The real dispatch interprets points against the frame the client showed:
 * packets 5.. went out at half the size on another output, 10.. at half the
 * size on this one, 50.. at the current size, and 100 is the next to be
 * sent. */
static void sync_geometry(HostCtx *ctx, uint32_t w, uint32_t h,
                          const MdCaptureRect *r, uint32_t first_seq) {
    MdCaptureCtx cap = { .output_rect = *r };
    const MdFrame f = { .width = w, .height = h };
    (void)host_capture_sync_frame(&ctx->sync, &cap, &f); /* no encoder */
    MD_CHECK(ctx->sync.width == w && ctx->sync.rect.x == r->x);
    host_capture_sync_packet_sent(&ctx->sync,
                                  host_capture_sync_frame_tag(&ctx->sync),
                                  first_seq);
}
static void check_frame_tags(HostCtx *ctx) {
    const MdCaptureRect r = { 0, 0, 1280, 720 }, other = { 1280, 0, 1280, 720 };
    const uint16_t W = TEST_FRAME_W, H = TEST_FRAME_H;
    ctx->pkt_seq = 0;
    host_capture_sync_init(&ctx->sync, NULL, NULL, NULL, &ctx->pkt_seq,
                           W / 2, H / 2, &other);
    host_capture_sync_packet_sent(&ctx->sync,
                                  host_capture_sync_frame_tag(&ctx->sync), 5);
    sync_geometry(ctx, W / 2, H / 2, &r, 10);
    sync_geometry(ctx, W, H, &r, 50);
    ctx->pkt_seq = 100;
    MD_CHECK(dispatch_move(ctx, 320, 180, W, H, 60, false));
    MD_CHECK(move_x == 320 && move_y == 180);
    MD_CHECK(!dispatch_move(ctx, 320, 180, W, H, 60, true));   /* untagged */
    MD_CHECK(!dispatch_move(ctx, 320, 180, 0, 0, 60, false));  /* no frame */
    MD_CHECK(!dispatch_move(ctx, 320, 180, 1280, 720, 60, false));
    MD_CHECK(!dispatch_move(ctx, (int16_t)W, 0, W, H, 60, false));
    MD_CHECK(!dispatch_move(ctx, 320, 180, W, H, 100, false)); /* not sent */
    MD_CHECK(!dispatch_move(ctx, 320, 180, W, H, 20, false));  /* wrong size */
    MD_CHECK(dispatch_move(ctx, 160, 90, W / 2, H / 2, 20, false));
    MD_CHECK(move_x == 320 && move_y == 180);                  /* rescaled */
    MD_CHECK(!dispatch_move(ctx, 160, 90, W / 2, H / 2, 4, false)); /* older */
    /* Another output at the same size: not translatable. */
    MD_CHECK(!dispatch_move(ctx, 160, 90, W / 2, H / 2, 7, false));
    MD_CHECK(ctx->sync.points_translated == 1);
    MdSession *session = ctx->session;
    const HostCtx fresh = context(session);
    ctx->sync = fresh.sync;
    ctx->pkt_seq = 0;
}
static void test_candidate_and_second_client(void) {
    MdSession s;
    MdSessionAccept acc = reserve(&s, MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT);
    Pair first = pair();
    HostCtx ctx = context(&s);
    MD_CHECK(pthread_mutex_init(&ctx.client_mu, NULL) == 0);
    uint8_t byte = 0;
    MdEncodedPacket pkt = { .data = &byte, .size = 1 };
    on_encoded(&pkt, &ctx); host_send_initial_tree(&ctx);
    MD_CHECK(tree_calls == 0);
    MdPollFd fd = { .fd = md_stream_get_fd(first.peer), .events = POLLIN };
    MD_CHECK(md_socket_poll(&fd, 1, 0) == 0);
    send_proof(first, &acc);
    MD_CHECK(host_bind_candidate(&ctx, first.host) == 0);
    MD_CHECK(ctx.client == NULL); /* ACK precedes publication. */
    ack(first, acc.granted);
    ctx.client = first.host;
    uint8_t move[MD_INPUT_EVENT_HDR_LEN + MD_INPUT_EVENT_TAG_LEN];
    MD_CHECK(md_input_event_pack(move, sizeof(move), MD_INPUT_EV_MOVE, 0, 1, 2,
                                 NULL, 0, TEST_FRAME_W, TEST_FRAME_H, 0));
    MdPacketHeader allowed = { .type = MD_PKT_INPUT, .payload_len = sizeof(move) };
    unsigned moves = input_calls, actions = action_calls;
    handle_client_packet(&ctx, &allowed, move);
    allowed.type = MD_PKT_ACTION;
    handle_client_packet(&ctx, &allowed, move);
    MD_CHECK(input_calls == moves + 1 && action_calls == actions + 1);
    check_frame_tags(&ctx);
    Pair second = pair();
    send_proof(second, &acc); /* Knowledge of a consumed token is useless. */
    MD_CHECK(host_bind_candidate(&ctx, second.host) < 0);
    closed(second);
    MD_CHECK(ctx.client == first.host && md_stream_has_capability(first.host, MD_CAP_VIDEO));
    MdPacketHeader ping = { .type = MD_PKT_PING };
    handle_client_packet(&ctx, &ping, NULL);
    MdPacketHeader pong; uint8_t *data = NULL;
    MD_CHECK(md_stream_recv(first.peer, &pong, &data, 1000) == 0 && pong.type == MD_PKT_PONG);
    free(data); destroy_pair(second);
    ctx.client = NULL; destroy_pair(first);
    Pair replay = pair(); send_proof(replay, &acc);
    MD_CHECK(host_bind_candidate(&ctx, replay.host) < 0);
    closed(replay); destroy_pair(replay);
    pthread_mutex_destroy(&ctx.client_mu);
}
static void test_unbound_dispatch(void) {
    for (unsigned action = 0; action < 2; action++) {
        MdSession s; reserve(&s, MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT);
        HostCtx ctx = context(&s);
        Pair p = pair(); ctx.client = p.host;
        uint8_t input[MD_INPUT_EVENT_HDR_LEN + MD_INPUT_EVENT_TAG_LEN];
        MD_CHECK(md_input_event_pack(input, sizeof(input), MD_INPUT_EV_MOVE, 0, 1, 2,
                                     NULL, 0, TEST_FRAME_W, TEST_FRAME_H, 0));
        MdPacketHeader hdr = { .type = action ? MD_PKT_ACTION : MD_PKT_INPUT,
                               .payload_len = sizeof(input) };
        unsigned moves = input_calls, actions = action_calls;
        handle_client_packet(&ctx, &hdr, input);
        MD_CHECK(moves == input_calls && actions == action_calls);
        closed(p); destroy_pair(p);
    }
}
static void test_bad_proofs(void) {
    MdSession s;
    MdSessionAccept acc = reserve(&s, MD_CAP_VIDEO);
    HostCtx ctx = context(&s);
    Pair wrong = pair();
    MdSessionAccept bad = acc; strcpy(bad.session_id, "wrong-id");
    send_proof(wrong, &bad);
    MD_CHECK(host_bind_candidate(&ctx, wrong.host) < 0 && s.state == MD_SESSION_NEGOTIATING);
    closed(wrong); destroy_pair(wrong);
    wrong = pair(); bad = acc; bad.stream_token[0] = bad.stream_token[0] == '0' ? '1' : '0';
    send_proof(wrong, &bad);
    MD_CHECK(host_bind_candidate(&ctx, wrong.host) < 0);
    closed(wrong); destroy_pair(wrong);
    wrong = pair(); /* Old parseable session_accept is not a proof. */
    char *legacy = md_session_accept_to_json(&acc);
    MD_CHECK(md_stream_send(wrong.peer, MD_PKT_SESSION_INFO, 0,
                            (uint8_t *)legacy, (uint32_t)strlen(legacy)) == 0);
    free(legacy);
    MD_CHECK(host_bind_candidate(&ctx, wrong.host) < 0);
    closed(wrong); destroy_pair(wrong);
    wrong = pair(); /* Oversize rejected before reading its body. */
    uint8_t header[MD_PACKET_HEADER_SIZE];
    MdPacketHeader hdr = { .version = MD_PROTOCOL_VERSION, .type = MD_PKT_SESSION_INFO,
                          .payload_len = MD_SESSION_JSON_MAX + 1 };
    MD_CHECK(md_packet_header_write(&hdr, header, sizeof(header)) > 0);
    MD_CHECK(md_socket_send(md_stream_get_fd(wrong.peer), header, sizeof(header), 0) == sizeof(header));
    MD_CHECK(host_bind_candidate(&ctx, wrong.host) < 0);
    closed(wrong); destroy_pair(wrong);
    wrong = pair(); send_proof(wrong, &acc); s.stream_deadline_ms = 0;
    MD_CHECK(host_bind_candidate(&ctx, wrong.host) < 0);
    MD_CHECK(s.state != MD_SESSION_ACTIVE && !s.stream_token[0]);
    closed(wrong); destroy_pair(wrong);
}
static void test_denied_capabilities(void) {
    for (unsigned action = 0; action < 2; action++) {
        MdSession s;
        MdSessionAccept acc = reserve(&s, MD_CAP_AGENT); /* No video/input. */
        HostCtx ctx = context(&s);
        MD_CHECK(pthread_mutex_init(&ctx.client_mu, NULL) == 0);
        Pair p = pair(); send_proof(p, &acc);
        MD_CHECK(host_bind_candidate(&ctx, p.host) == 0); ack(p, MD_CAP_AGENT);
        ctx.client = p.host;
        uint8_t byte = 0; MdEncodedPacket pkt = { .data = &byte, .size = 1 };
        on_encoded(&pkt, &ctx);
        MdPollFd fd = { .fd = md_stream_get_fd(p.peer), .events = POLLIN };
        MD_CHECK(md_socket_poll(&fd, 1, 0) == 0);
        unsigned trees = tree_calls; host_send_initial_tree(&ctx);
        MD_CHECK(tree_calls == trees + 1);
        uint8_t input[MD_INPUT_EVENT_HDR_LEN + MD_INPUT_EVENT_TAG_LEN];
        MD_CHECK(md_input_event_pack(input, sizeof(input), MD_INPUT_EV_MOVE, 0, 12, 34,
                                     NULL, 0, TEST_FRAME_W, TEST_FRAME_H, 0));
        MdPacketHeader hdr = { .type = action ? MD_PKT_ACTION : MD_PKT_INPUT,
                               .payload_len = sizeof(input) };
        unsigned moves = input_calls, actions = action_calls;
        handle_client_packet(&ctx, &hdr, input);
        MD_CHECK(input_calls == moves && action_calls == actions);
        closed(p); destroy_pair(p);
        pthread_mutex_destroy(&ctx.client_mu);
    }
    MdSession s; MdSessionAccept acc = reserve(&s, MD_CAP_VIDEO);
    HostCtx ctx = context(&s);
    Pair p = pair(); send_proof(p, &acc);
    MD_CHECK(host_bind_candidate(&ctx, p.host) == 0); ack(p, MD_CAP_VIDEO);
    ctx.client = p.host;
    unsigned trees = tree_calls; host_send_initial_tree(&ctx); MD_CHECK(tree_calls == trees);
    MD_CHECK(pthread_mutex_init(&ctx.client_mu, NULL) == 0);
    uint8_t byte = 42; MdEncodedPacket frame = { .data = &byte, .size = 1 };
    on_encoded(&frame, &ctx);
    MdPacketHeader video; uint8_t *payload = NULL;
    MD_CHECK(md_stream_recv(p.peer, &video, &payload, 1000) == 0);
    MD_CHECK(video.type == MD_PKT_VIDEO_FRAME && video.payload_len == 1 && payload[0] == byte);
    free(payload);
    pthread_mutex_destroy(&ctx.client_mu);
    MdPacketHeader hdr = { .type = MD_PKT_SESSION_INFO }; /* Duplicate bind. */
    handle_client_packet(&ctx, &hdr, NULL); closed(p); destroy_pair(p);
    acc = reserve(&s, MD_CAP_INPUT); p = pair(); send_proof(p, &acc);
    ctx = context(&s);
    MD_CHECK(host_bind_candidate(&ctx, p.host) == 0); ack(p, MD_CAP_INPUT);
    ctx.client = p.host;
    unsigned actions = action_calls;
    hdr.type = MD_PKT_ACTION;
    handle_client_packet(&ctx, &hdr, NULL);
    MD_CHECK(action_calls == actions); closed(p); destroy_pair(p);
}
typedef struct { MdStreamServer *server; MdStream *stream; } AttendedTlsAccept;
static void *attended_tls_accept(void *data) {
    AttendedTlsAccept *accept = data;
    accept->stream = md_stream_server_accept(accept->server, 5000);
    return NULL;
}

/* Exercise the real human-grant branch with the listener's actual certificate,
 * then bind and tear down pinned TLS through both revocation paths. No desktop,
 * relay, fabricated fingerprint or token-before-pin shortcut in this case. */
static void test_attended_tls_revocation(void) {
    for (unsigned cancel = 0; cancel < 2; ++cancel) {
        MdHostAccess access;
        MD_CHECK(md_host_access_init(&access) == 0);
        MdSession session; md_session_init(&session);
        HostCtx ctx = {.session = &session, .access = &access};
        pthread_mutex_init(&ctx.client_mu, NULL);
        ctx.session_req_ch = go_channel_create(1);
        const char *peer = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
        MdSessionRequest request = {.capabilities = MD_CAP_VIDEO | MD_CAP_INPUT,
                                    .tree_format = MD_TREE_FORMAT_COMPACT};
        MD_CHECK(md_session_request_prepare(&request, time(NULL), 60) == 0);
        MD_CHECK(md_host_access_enqueue(&access, peer, &request, time(NULL)) == 0);
        MdHostAccessSnapshot v;
        md_host_access_snapshot(&access, time(NULL), &v);
        MD_CHECK(md_host_access_decide(&access, v.generation, v.revision,
            request.request_id, true, false, MD_CAP_VIDEO, MD_HOST_ACCESS_CAPS,
            time(NULL), NULL) == 0);
        MD_CHECK(host_has_request(&ctx) && ctx.human_approved);
        ctx.pending_req.stream_tls = false;
        MD_CHECK(host_accept_request(&ctx, NULL, "legacy-attended", true) < 0);
        MD_CHECK(session.state == MD_SESSION_IDLE && !session.stream_token[0]);
        ctx.pending_req.stream_tls = true;
        MdStreamTlsConfig tls = {.enabled = true};
        MdStreamServer *server = md_stream_server_create_tls("::1", 0, &tls);
        MD_CHECK(server);
        MD_CHECK(md_stream_server_tls_fingerprint(server, ctx.tls_cert_sha256) == 0);
        MD_CHECK(host_accept_request(&ctx, NULL, "negotiated-id", true) == 0);
        MD_CHECK(host_publish_session_accept(&ctx, NULL) == 0);
        MD_CHECK(!strcmp(published.tls_cert_sha256, ctx.tls_cert_sha256));
        MD_CHECK(!strcmp(published.request_id, request.request_id));
        MD_CHECK(published.granted == MD_CAP_VIDEO && published.stream_token[0]);
        AttendedTlsAccept accept = {.server = server};
        pthread_t thread;
        MD_CHECK(pthread_create(&thread, NULL, attended_tls_accept, &accept) == 0);
        tls.cert_sha256 = published.tls_cert_sha256;
        Pair p = {.peer = md_stream_connect_tls("::1", md_stream_server_port(server), 5000, &tls)};
        pthread_join(thread, NULL); p.host = accept.stream;
        MD_CHECK(p.peer && p.host && md_stream_is_tls(p.peer) && md_stream_is_tls(p.host));
        send_proof(p, &published); /* only after the pinned handshake above */
        MD_CHECK(host_bind_candidate(&ctx, p.host) == 0);
        ack(p, MD_CAP_VIDEO);
        MD_CHECK(!session.stream_token[0]);
        ctx.client = p.host;
        if (cancel) {
            char json[128];
            snprintf(json, sizeof(json),
                "{\"type\":\"session_cancel\",\"v\":1,\"request_id\":\"%s\"}", request.request_id);
            MdNostrDmInfo info = {.live = true};
            host_on_dm(peer, json, &info, &ctx);
        } else {
            md_host_access_snapshot(&access, time(NULL), &v);
            MD_CHECK(md_host_access_kick(&access, v.generation, v.revision, session.session_id) == 0);
        }
        MD_CHECK(!md_host_access_valid(&access, request.request_id));
        /* Revocation blocks even a harmless queued packet and closes TLS. */
        MdPacketHeader hdr = {.type = MD_PKT_PING};
        handle_client_packet(&ctx, &hdr, NULL);
        closed(p);
        host_finish_client(&ctx, p.host);
        MD_CHECK(!ctx.client && session.state == MD_SESSION_IDLE && !session.stream_token[0]);
        md_host_access_snapshot(&access, time(NULL), &v);
        MD_CHECK(!v.occupied && !ctx.session_requested);
        md_stream_destroy(p.peer);
        md_stream_server_destroy(server);
        go_channel_close(ctx.session_req_ch); go_channel_unref(ctx.session_req_ch);
        pthread_mutex_destroy(&ctx.client_mu);
        md_host_access_destroy(&access);
    }
}

static void test_legacy_tls_refusal(void) {
    MdSession session; md_session_init(&session);
    HostCtx ctx = context(&session);
    MD_CHECK(md_session_request_prepare(&ctx.pending_req, time(NULL), 30) == 0);
    ctx.pending_req.stream_tls = false;
    unsigned before = publish_calls;
    MD_CHECK(host_accept_request(&ctx, (MdNostr *)(uintptr_t)1, "legacy", true) < 0);
    MD_CHECK(!session.stream_token[0]);
    MD_CHECK(host_publish_session_accept(&ctx, NULL) < 0);
    MD_CHECK(publish_calls == before && !session.stream_token[0]);
}

static void test_late_publication(void) {
    MdSession s; md_session_init(&s);
    MD_CHECK(md_session_accept(&s, "negotiated-id", MD_CAP_VIDEO) == 0);
    HostCtx ctx = context(&s);
    Pair p = pair(); /* Listener ready before acceptance. */
    unsigned before = publish_calls;
    MD_CHECK(!s.stream_token[0]); /* Backend startup cannot spend token lifetime. */
    /* An uncorrelated request cannot be answered or mint a token (md-6ns). */
    MD_CHECK(host_publish_session_accept(&ctx, NULL) < 0);
    MD_CHECK(publish_calls == before && !s.stream_token[0]);
    MD_CHECK(md_session_request_prepare(&ctx.pending_req, (int64_t)time(NULL), 30) == 0);
    MD_CHECK(host_publish_session_accept(&ctx, NULL) == 0);
    MD_CHECK(publish_calls == before + 1 && s.stream_token[0]);
    MD_CHECK(strcmp(published.request_id, ctx.pending_req.request_id) == 0);
    send_proof(p, &published);
    MD_CHECK(host_bind_candidate(&ctx, p.host) == 0); ack(p, MD_CAP_VIDEO);
    destroy_pair(p);
}
static void test_expiry_and_strict_json(void) {
    MdSession s; MdSessionAccept acc = reserve(&s, MD_CAP_VIDEO);
    char *proof = md_session_bind_to_json(&acc);
    MD_CHECK(md_session_consume_binding(&s, (uint8_t *)proof, strlen(proof), s.stream_deadline_ms) < 0);
    MD_CHECK(s.state != MD_SESSION_ACTIVE && !s.stream_token[0]); free(proof);
    acc = reserve(&s, MD_CAP_VIDEO); proof = md_session_bind_to_json(&acc);
    size_t len = strlen(proof);
    char *trailing = malloc(len + 3); MD_CHECK(trailing);
    memcpy(trailing, proof, len); memcpy(trailing + len, "{}", 3);
    MD_CHECK(md_session_consume_binding(&s, (uint8_t *)trailing, len + 2, 1) < 0);
    proof[len / 2] = '\0';
    MD_CHECK(md_session_consume_binding(&s, (uint8_t *)proof, len, 1) < 0);
    free(trailing); free(proof);
}
/* Real private-relay policy -> negotiated token -> production stream gates.
 * Desktop boundaries above are instrumented; no GUI backend is started. */
static void test_relay_grant(const char *url, uint32_t expected) {
    const char *urls[] = {url};
    MdNostrConfig cfg = {
        .sk_hex = "0000000000000000000000000000000000000000000000000000000000000001",
        .relay_urls = urls, .relay_count = 1,
    };
    const char *client = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";
    MdNostr *n = md_nostr_create(&cfg, NULL);
    MD_CHECK(n);
    MdAllowlistEntry *entries = NULL;
    size_t count = 0;
    MD_CHECK(md_nostr_allowlist_snapshot(n, &entries, &count) == 0);
    free(entries);
    uint32_t granted = md_nostr_granted_capabilities(n, client, UINT32_MAX);
    MD_CHECK(granted == expected);
    MD_CHECK(md_nostr_granted_capabilities(n, client, MD_CAP_INPUT) ==
             (expected & MD_CAP_INPUT));
    MD_CHECK(md_nostr_granted_capabilities(n, client, 0) == 0);
    MdSession negotiation; md_session_init(&negotiation);
    HostCtx request = context(&negotiation);
    snprintf(request.pending_client_pk, sizeof(request.pending_client_pk), "%s", client);
    request.pending_req.capabilities = MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT;
    request.pending_req.tree_format = MD_TREE_FORMAT_COMPACT;
    MD_CHECK(host_accept_request(&request, n, "negotiated-id", false) == (expected ? 0 : -1));
    MD_CHECK(negotiation.state == (expected ? MD_SESSION_NEGOTIATING : MD_SESSION_IDLE));
    MD_CHECK(negotiation.capabilities == expected && !negotiation.stream_token[0]);
    for (unsigned action = 0; granted == MD_CAP_VIDEO && action < 2; action++) {
        MdSession s; md_session_init(&s);
        HostCtx ctx = request; ctx.session = &s;
        MD_CHECK(host_accept_request(&ctx, n, "negotiated-id", false) == 0);
        MdSessionAccept acc;
        MD_CHECK(md_session_prepare_binding(&s, &acc, (uint64_t)md_now_ms()) == 0);
        Pair p = pair(); send_proof(p, &acc);
        MD_CHECK(host_bind_candidate(&ctx, p.host) == 0);
        ack(p, MD_CAP_VIDEO); ctx.client = p.host;
        unsigned trees = tree_calls; host_send_initial_tree(&ctx);
        MD_CHECK(tree_calls == trees);
        uint8_t input[MD_INPUT_EVENT_HDR_LEN + MD_INPUT_EVENT_TAG_LEN];
        MD_CHECK(md_input_event_pack(input, sizeof(input), MD_INPUT_EV_MOVE, 0, 1, 2,
                                     NULL, 0, TEST_FRAME_W, TEST_FRAME_H, 0));
        MdPacketHeader hdr = { .type = action ? MD_PKT_ACTION : MD_PKT_INPUT,
                               .payload_len = sizeof(input) };
        unsigned moves = input_calls, actions = action_calls;
        handle_client_packet(&ctx, &hdr, input);
        MD_CHECK(input_calls == moves && action_calls == actions);
        closed(p); destroy_pair(p);
    }
    if (granted == MD_CAP_VIDEO) {
        /* An acknowledged live edit on this same bridge must affect the next
         * production acceptance, without minting a reservation. */
        MD_CHECK(md_nostr_allowlist_add(n, client, "none") == 0);
        md_session_init(&negotiation);
        MD_CHECK(host_accept_request(&request, n, "next-session", false) < 0);
        MD_CHECK(negotiation.state == MD_SESSION_IDLE && !negotiation.stream_token[0]);
    } else if (!granted) {
        /* Editing another principal must not turn malformed/truncated scope
         * records into valid grants, either locally or after a fresh REQ. */
        MD_CHECK(md_nostr_allowlist_add(n,
            "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", "video") == 0);
        MD_CHECK(md_nostr_granted_capabilities(n, client, UINT32_MAX) == 0);
        md_nostr_destroy(n);
        n = md_nostr_create(&cfg, NULL); MD_CHECK(n);
        MD_CHECK(md_nostr_allowlist_snapshot(n, &entries, &count) == 0);
        free(entries);
        md_session_init(&negotiation);
        MD_CHECK(host_accept_request(&request, n, "reloaded-session", false) < 0);
        MD_CHECK(negotiation.state == MD_SESSION_IDLE && !negotiation.stream_token[0]);
    }
    md_nostr_destroy(n);
}
/* ── md-6ns: request correlation over a history-retaining private relay ──
 * Real host_on_dm/host_accept_request/host_publish_session_accept and real
 * NIP-17 bridges. The client probe applies the managed client's acceptance
 * rule (md_session_accept_matches_request). Waits are on DM callbacks; the
 * timed waits are deadlock bounds only. Throwaway keys 2 (host), 3 (client). */
#define HIST_HOST_SK "0000000000000000000000000000000000000000000000000000000000000002"
#define HIST_CLIENT_SK "0000000000000000000000000000000000000000000000000000000000000003"
static const char *hist_nip5f_socket;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    HostCtx *host;               /* host probe: forward to the real gate */
    MdSessionRequest req;        /* client probe: its current request */
    bool armed;
    MdSessionAccept accepted;
    unsigned matched, rejections, ignored_history, ignored_live, host_history, host_live;
} HistProbe;

static void hist_init(HistProbe *p, HostCtx *host) {
    memset(p, 0, sizeof(*p));
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->host = host;
}
static void hist_free(HistProbe *p) {
    OPENSSL_cleanse(p->accepted.stream_token, sizeof(p->accepted.stream_token));
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
}
static void hist_host_dm(const char *pk, const char *content,
                         const MdNostrDmInfo *info, void *ud) {
    HistProbe *p = ud;
    host_on_dm(pk, content, info, p->host);
    pthread_mutex_lock(&p->mu);
    if (info->live) p->host_live++; else p->host_history++;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}
static void hist_client_dm(const char *pk, const char *content,
                           const MdNostrDmInfo *info, void *ud) {
    HistProbe *p = ud;
    MdSessionAccept acc;
    MdSessionReject reject;
    (void)pk;
    if (md_session_reject_from_json(content, &reject) == 0) {
        pthread_mutex_lock(&p->mu);
        if (p->armed && md_session_reject_matches_request(&reject, &p->req,
                                                         info->rumor_created_at)) {
            p->rejections++;
            p->armed = false;
        }
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
        return;
    }
    if (md_session_accept_from_json(content, &acc) != 0) return;
    pthread_mutex_lock(&p->mu);
    if (p->armed && md_session_accept_matches_request(&acc, &p->req, info->rumor_created_at)) {
        p->accepted = acc;
        p->armed = false;
        p->matched++;
    } else if (info->live) {
        p->ignored_live++;
    } else {
        p->ignored_history++;
    }
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    OPENSSL_cleanse(acc.stream_token, sizeof(acc.stream_token));
}
static bool hist_wait(HistProbe *p, const unsigned *counter, unsigned target) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 20; /* deadlock bound, not a delivery timer */
    pthread_mutex_lock(&p->mu);
    int rc = 0;
    while (*counter < target && rc == 0)
        rc = pthread_cond_timedwait(&p->cv, &p->mu, &deadline);
    bool ok = *counter >= target;
    pthread_mutex_unlock(&p->mu);
    return ok;
}
static MdNostr *hist_bridge_n(const char **urls, int count, const char *sk,
                              MdNostrDmCallback cb, HistProbe *p) {
    MdSigner *signer = NULL;
    if (hist_nip5f_socket && !strcmp(sk, HIST_HOST_SK)) {
        signer = md_signer_create_nip5f(hist_nip5f_socket);
        MD_CHECK(signer);
    }
    MdNostrConfig cfg = { .sk_hex = signer ? NULL : sk, .signer = signer,
                          .relay_urls = urls, .relay_count = count,
                          .allowlist_open = true };
    MdNostrCallbacks cbs = { .on_dm = cb, .dm_userdata = p };
    MdNostr *n = md_nostr_create(&cfg, &cbs);
    md_signer_destroy(signer);
    MD_CHECK(n);
    /* The catch-up -> live boundary, as the host waits before announcing. */
    MD_CHECK(n && md_nostr_wait_history(n, 10000) == 0);
    return n;
}
static MdNostr *hist_bridge(const char *url, const char *sk, MdNostrDmCallback cb,
                            HistProbe *p) {
    const char *urls[] = {url};
    return hist_bridge_n(urls, 1, sk, cb, p);
}
static void hist_decoy(MdNostr *host, const char *client_pk, bool correlated) {
    MdSessionAccept other = { .granted = MD_CAP_VIDEO, .tree_format = MD_TREE_FORMAT_COMPACT };
    snprintf(other.session_id, sizeof(other.session_id), "%s",
             correlated ? "other-request" : "uncorrelated");
    memset(other.stream_token, 'a', 64);
    if (correlated) {
        MdSessionRequest elsewhere = {0};
        MD_CHECK(md_session_request_prepare(&elsewhere, (int64_t)time(NULL), 30) == 0);
        memcpy(other.request_id, elsewhere.request_id, sizeof(other.request_id));
    }
    char *json = md_session_accept_to_json(&other);
    MD_CHECK(json && md_nostr_send_session_accept(host, client_pk, json) == 0);
    free(json);
}

/* One negotiation by a new host and new client process pair on the same
 * relay. `host_history`/`client_history` are the stale DMs each must ignore. */
static void hist_session(const char *url, const char *session_id,
                         unsigned host_history, unsigned client_history, bool decoys) {
    MdSession s; md_session_init(&s);
    HostCtx ctx = context(&s);
    ctx.session_req_ch = go_channel_create(1);
    HistProbe hp, cp;
    hist_init(&hp, &ctx);
    hist_init(&cp, NULL);
    MdNostr *host = hist_bridge(url, HIST_HOST_SK, hist_host_dm, &hp);
    ctx.nostr = host;
    /* Earlier sessions' requests replay as history and never latch. */
    MD_CHECK(hist_wait(&hp, &hp.host_history, host_history));
    md_nostr_drain_dm(host);
    MD_CHECK(!host_has_request(&ctx));
    pthread_mutex_lock(&hp.mu);
    MD_CHECK(hp.host_live == 0);
    pthread_mutex_unlock(&hp.mu);

    /* Arm before the client bridge exists, so replayed acceptances are
     * judged against this request rather than an idle client. */
    MdSessionRequest req = { .capabilities = MD_CAP_VIDEO,
                             .tree_format = MD_TREE_FORMAT_COMPACT };
    MD_CHECK(md_session_request_prepare(&req, (int64_t)time(NULL), 30) == 0);
    pthread_mutex_lock(&cp.mu);
    cp.req = req;
    cp.armed = true;
    pthread_mutex_unlock(&cp.mu);
    MdNostr *client = hist_bridge(url, HIST_CLIENT_SK, hist_client_dm, &cp);
    const char *host_pk = md_nostr_get_npub(host), *client_pk = md_nostr_get_npub(client);
    MD_CHECK(hist_wait(&cp, &cp.ignored_history, client_history));
    md_nostr_drain_dm(client);
    MD_CHECK(cp.matched == 0 && cp.ignored_history == client_history);

    if (decoys) {
        /* Live, authentic host acceptances answering some other request. */
        hist_decoy(host, client_pk, true);
        hist_decoy(host, client_pk, false);
        MD_CHECK(hist_wait(&cp, &cp.ignored_live, 2));
        MD_CHECK(cp.matched == 0);
    }

    char *json = md_session_request_to_json(&req);
    MD_CHECK(json && md_nostr_send_session_request(client, host_pk, json) == 0);
    free(json);
    void *dummy = NULL;
    GoSelectCase cases[] = {
        { .op = GO_SELECT_RECEIVE, .chan = ctx.session_req_ch, .recv_buf = &dummy },
    };
    go_select_timeout(cases, 1, 20000); /* deadlock bound */
    MD_CHECK(ctx.session_requested);
    MD_CHECK(strcmp(ctx.pending_req.request_id, req.request_id) == 0);
    MD_CHECK(strcmp(ctx.pending_client_pk, client_pk) == 0);
    MD_CHECK(host_accept_request(&ctx, host, session_id, false) == 0);
    MD_CHECK(host_publish_session_accept(&ctx, NULL) == 0);
    MD_CHECK(hist_wait(&cp, &cp.matched, 1));
    MD_CHECK(cp.matched == 1 && strcmp(cp.accepted.session_id, session_id) == 0);
    MD_CHECK(strcmp(cp.accepted.request_id, req.request_id) == 0);
    MD_CHECK(CRYPTO_memcmp(cp.accepted.stream_token, s.stream_token, 64) == 0);

    /* Same-process admission rearms, but cannot answer the consumed request
     * again even if a live delivery bypasses the transport's event dedup. */
    host_rearm_request(&ctx);
    json = md_session_request_to_json(&req);
    const MdNostrDmInfo live = { .rumor_created_at = req.issued_at, .live = true };
    host_on_dm(client_pk, json, &live, &ctx);
    free(json);
    MD_CHECK(!host_has_request(&ctx));
    MD_CHECK(md_session_request_prepare(&req, (int64_t)time(NULL), 30) == 0);
    json = md_session_request_to_json(&req);
    const MdNostrDmInfo next = { .rumor_created_at = req.issued_at, .live = true };
    host_on_dm(client_pk, json, &next, &ctx);
    free(json);
    MD_CHECK(host_has_request(&ctx));
    MD_CHECK(!strcmp(ctx.pending_req.request_id, req.request_id));

    md_nostr_destroy(client);
    md_nostr_destroy(host);
    go_channel_close(ctx.session_req_ch);
    go_channel_unref(ctx.session_req_ch);
    hist_free(&hp);
    hist_free(&cp);
    md_session_reset(&s);
}

static void test_relay_history(const char *url) {
    /* 1: empty relay. 2: request A and accept(A) replay; live decoys too.
     * 3: requests A, B and accepts A, B plus both decoys replay. */
    hist_session(url, "session-one", 0, 0, false);
    hist_session(url, "session-two", 1, 1, true);
    hist_session(url, "session-three", 2, 4, false);
}

/* A multi-relay race: the client's live request reaches relay 1 before the
 * host subscribes (history there), and the identical gift wrap reaches relay 2
 * only after the host's EOSE (live there). Dedup must not let the history copy
 * suppress the live one. The driver replicates the wrap on "inject". */
static void test_two_relay_upgrade(const char *url1, const char *url2) {
    MdSession s; md_session_init(&s);
    HostCtx ctx = context(&s);
    ctx.session_req_ch = go_channel_create(1);
    HistProbe hp, cp;
    hist_init(&hp, &ctx);
    hist_init(&cp, NULL);
    MdSessionRequest req = { .capabilities = MD_CAP_VIDEO,
                             .tree_format = MD_TREE_FORMAT_COMPACT };
    MD_CHECK(md_session_request_prepare(&req, (int64_t)time(NULL), 30) == 0);
    cp.req = req;
    cp.armed = true;
    MdNostr *client = hist_bridge(url1, HIST_CLIENT_SK, hist_client_dm, &cp);
    char host_pk[65];
    char *derived = md_nostr_get_pubkey(HIST_HOST_SK);
    MD_CHECK(derived);
    snprintf(host_pk, sizeof(host_pk), "%s", derived ? derived : "");
    free(derived);
    char *json = md_session_request_to_json(&req);
    MD_CHECK(json && md_nostr_send_session_request(client, host_pk, json) == 0);
    free(json);

    const char *urls[] = {url1, url2};
    MdNostr *host = hist_bridge_n(urls, 2, HIST_HOST_SK, hist_host_dm, &hp);
    ctx.nostr = host;
    MD_CHECK(hist_wait(&hp, &hp.host_history, 1));
    md_nostr_drain_dm(host);
    MD_CHECK(!host_has_request(&ctx));
    pthread_mutex_lock(&hp.mu);
    MD_CHECK(hp.host_live == 0);
    pthread_mutex_unlock(&hp.mu);

    printf("md6ns: inject\n");
    fflush(stdout);
    void *dummy = NULL;
    GoSelectCase cases[] = {
        { .op = GO_SELECT_RECEIVE, .chan = ctx.session_req_ch, .recv_buf = &dummy },
    };
    go_select_timeout(cases, 1, 20000); /* deadlock bound */
    MD_CHECK(hist_wait(&hp, &hp.host_live, 1)); /* probe counts after the gate */
    MD_CHECK(ctx.session_requested && hp.host_live == 1 && hp.host_history == 1);
    MD_CHECK(strcmp(ctx.pending_req.request_id, req.request_id) == 0);
    MD_CHECK(host_accept_request(&ctx, host, "two-relay", false) == 0);
    MD_CHECK(host_publish_session_accept(&ctx, NULL) == 0);
    MD_CHECK(hist_wait(&cp, &cp.matched, 1));
    MD_CHECK(strcmp(cp.accepted.session_id, "two-relay") == 0);

    md_nostr_destroy(client);
    md_nostr_destroy(host);
    /* Exactly one live delivery: no repeat after the upgrade. */
    MD_CHECK(hp.host_live == 1 && hp.host_history == 1);
    go_channel_close(ctx.session_req_ch);
    go_channel_unref(ctx.session_req_ch);
    hist_free(&hp);
    hist_free(&cp);
    md_session_reset(&s);
}

/* Host gate without a relay: history and stale/uncorrelated requests never
 * latch. (A fresh live request reaches the allowlist; none here.) */
static void test_request_gate(void) {
    MdSession s; md_session_init(&s);
    HostCtx ctx = context(&s);
    const char *pk = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";
    int64_t now = (int64_t)time(NULL);
    MdSessionRequest req = { .capabilities = MD_CAP_VIDEO };
    MD_CHECK(md_session_request_prepare(&req, now, 30) == 0);
    char *json = md_session_request_to_json(&req);
    MD_CHECK(json);
    MdNostrDmInfo history = { .rumor_created_at = now, .live = false };
    host_on_dm(pk, json, &history, &ctx);
    MdNostrDmInfo skewed = { .rumor_created_at = now - 3600, .live = true };
    host_on_dm(pk, json, &skewed, &ctx);
    free(json);
    MdSessionRequest stale = req;
    stale.issued_at = now - 120; stale.expires_at = now - 90;
    json = md_session_request_to_json(&stale);
    MdNostrDmInfo live_old = { .rumor_created_at = stale.issued_at, .live = true };
    host_on_dm(pk, json, &live_old, &ctx);
    free(json);
    host_on_dm(pk, "{\"type\":\"session_request\",\"v\":1,\"capabilities\":[\"video\"]}",
               &(MdNostrDmInfo){ .rumor_created_at = now, .live = true }, &ctx);
    MD_CHECK(!ctx.session_requested && !ctx.pending_req.request_id[0]);
}

/* Rearming discards a consumed grant, not the last request's replay fence. */
static void test_rearm_request(void) {
    MdSession s;
    MdSessionAccept acc = reserve(&s, MD_CAP_VIDEO);
    HostCtx ctx = context(&s);
    ctx.session_requested = 1;
    MD_CHECK(md_session_request_prepare(&ctx.pending_req, (int64_t)time(NULL), 30) == 0);
    MD_CHECK(host_remember_request(&ctx, &ctx.pending_req, (int64_t)time(NULL)));
    char previous[MD_SESSION_REQUEST_ID_LEN + 1];
    memcpy(previous, ctx.pending_req.request_id, sizeof(previous));
    strcpy(ctx.pending_client_pk, "old-peer");
    MD_CHECK(host_has_request(&ctx));
    host_rearm_request(&ctx);
    MD_CHECK(!host_has_request(&ctx));
    MD_CHECK(!ctx.pending_req.request_id[0] && !ctx.pending_client_pk[0]);
    MD_CHECK(!strcmp(ctx.seen_requests[0].id, previous));
    MD_CHECK(s.state == MD_SESSION_IDLE && !s.stream_token[0]);
    Pair replay = pair();
    send_proof(replay, &acc);
    MD_CHECK(host_bind_candidate(&ctx, replay.host) < 0);
    closed(replay);
    destroy_pair(replay);
    OPENSSL_cleanse(acc.stream_token, sizeof(acc.stream_token));
}


/* No desktop: real HAL calls with scripted startup/first-frame outcomes. */
typedef struct {
    int start_result, frame_result;
    uint32_t width, height, timeout;
    unsigned pulls, releases;
} AdmissionCapture;
static int admission_start(MdCaptureCtx *cap) {
    AdmissionCapture *m = cap->backend_data;
    return m->start_result;
}
static int admission_frame(MdCaptureCtx *cap, MdFrame *frame, uint32_t timeout) {
    AdmissionCapture *m = cap->backend_data;
    static uint8_t pixel[640 * 360 * 4];
    m->pulls++; m->timeout = timeout;
    *frame = (MdFrame){ .width = m->width, .height = m->height,
                       .stride = m->width * 4, .format = MD_PIX_CAPTURE_RGBA,
                       .buf_type = MD_BUF_CPU, .data = pixel,
                       .data_size = (size_t)m->width * m->height * 4 };
    return m->frame_result;
}
static void admission_release(MdCaptureCtx *cap, MdFrame *frame) {
    (void)frame;
    ((AdmissionCapture *)cap->backend_data)->releases++;
}
static void test_capture_admission(void) {
    const MdCaptureBackend backend = { .start = admission_start,
        .get_frame = admission_frame, .release_frame = admission_release };
    AdmissionCapture cases[] = {
        {.start_result = -1}, /* portal denial/timeout */
        {.frame_result = MD_CAPTURE_ERROR}, /* broken/never delivered */
        {.frame_result = MD_CAPTURE_IDLE}, /* size alone is not readiness */
        {.width = 1, .height = 1}, /* unusable first frame */
        {.width = 640, .height = 360},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        MdCaptureCtx cap = {.vtable = &backend, .backend_data = &cases[i]};
        uint32_t w = 0, h = 0;
        HostCtx snapshot = {0};
        bool ready = host_start_capture(&cap, &w, &h, &snapshot) == 0;
        MD_CHECK(ready == (i == 4));
        MD_CHECK(cases[i].pulls == (i == 0 ? 0 : 1));
        MD_CHECK(cases[i].releases == (i >= 3 ? 1 : 0));
        if (i) MD_CHECK(cases[i].timeout == HOST_CAPTURE_FIRST_FRAME_WAIT_MS);
        MdSession session; md_session_init(&session);
        HostCtx ctx = {.tls_cert_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", .session = &session, .capture_required = true,
                       .capture_ready = ready};
        MD_CHECK(md_session_request_prepare(&ctx.pending_req, (int64_t)time(NULL), 30) == 0);
        MD_CHECK(md_session_accept(&session, "capture-admission", MD_CAP_VIDEO) == 0);
        unsigned accepts = publish_calls, rejects = reject_calls;
        if (ready) {
            MD_CHECK(w == 640 && h == 360);
            MD_CHECK(host_publish_session_accept(&ctx, NULL) == 0);
            MD_CHECK(publish_calls == accepts + 1 && session.stream_token[0]);
        } else {
            MD_CHECK(host_publish_session_accept(&ctx, NULL) < 0);
            MD_CHECK(publish_calls == accepts && !session.stream_token[0]);
            MD_CHECK(host_reject_request(&ctx, "capture_unavailable") == 0);
            MD_CHECK(reject_calls == rejects + 1 && session.state == MD_SESSION_IDLE);
            MD_CHECK(!strcmp(rejected.request_id, ctx.pending_req.request_id));
            MD_CHECK(md_session_reject_matches_request(&rejected, &ctx.pending_req,
                                                       ctx.pending_req.issued_at));
        }
        md_session_reset(&session);
        free(snapshot.cached_frame.data);
    }
    uint32_t w = 0, h = 0;
    MD_CHECK(host_start_capture(NULL, &w, &h, NULL) < 0);
    MdSession session; md_session_init(&session);
    HostCtx ctx = {.tls_cert_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", .session = &session}; /* explicit --no-capture fixture */
    MD_CHECK(md_session_request_prepare(&ctx.pending_req, (int64_t)time(NULL), 30) == 0);
    MD_CHECK(md_session_accept(&session, "no-capture-fixture", MD_CAP_VIDEO) == 0);
    MD_CHECK(host_publish_session_accept(&ctx, NULL) == 0);
    MD_CHECK(host_binding_pending(&ctx, session.stream_deadline_ms - 1));
    MD_CHECK(!host_binding_pending(&ctx, session.stream_deadline_ms + 1));
    md_session_reset(&session);
    MD_CHECK(md_session_accept(&session, "worker-failed", MD_CAP_VIDEO) == 0);
    ctx.capture_required = true; /* failed pthread_create, no admission */
    MD_CHECK(host_publish_session_accept(&ctx, NULL) < 0);
    ctx.capture_ready = true;
    atomic_store(&ctx.capture_failed, true); /* exhausted watchdog */
    MD_CHECK(host_publish_session_accept(&ctx, NULL) < 0);
    MD_CHECK(!session.stream_token[0]);
}

/* After the sole startup frame, the backend has no desktop damage. A
 * condition wait models its blocking get_frame; the socket packet is the
 * completion event, never a guessed sleep or retry-until-encode loop. */
static pthread_mutex_t idle_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle_cv = PTHREAD_COND_INITIALIZER;
static int idle_frame(MdCaptureCtx *cap, MdFrame *frame, uint32_t timeout) {
    (void)cap; (void)frame;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)timeout * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&idle_mu);
    pthread_cond_timedwait(&idle_cv, &idle_mu, &deadline);
    pthread_mutex_unlock(&idle_mu);
    return MD_CAPTURE_IDLE;
}
static void idle_decoded(const MdDecodedFrame *frame, void *data) {
    unsigned *count = data;
    MD_CHECK(frame->width == 640 && frame->height == 360 && frame->data);
    (*count)++;
}
static void test_idle_capture_rebind(void) {
    MdSession session;
    md_session_init(&session);
    HostCtx ctx = {.tls_cert_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", .session = &session};
    pthread_mutex_init(&ctx.client_mu, NULL);
    AdmissionCapture source = {.width = 640, .height = 360};
    const MdCaptureBackend startup = {.start = admission_start,
        .get_frame = admission_frame, .release_frame = admission_release};
    const MdCaptureBackend idle = {.get_frame = idle_frame};
    MdCaptureCtx cap = {.vtable = &startup, .backend_data = &source};
    uint32_t w = 0, h = 0;
    MD_CHECK(host_start_capture(&cap, &w, &h, &ctx) == 0);
    MD_CHECK(source.releases == 1 && ctx.cached_frame.data_size);
    cap.vtable = &idle;
    MdEncoderConfig cfg = {.width = w, .height = h, .fps = 30,
                            .bitrate = MD_ENCODER_DEFAULT_BITRATE};
    MdEncoder *encoder = md_encoder_create(&cfg);
    MD_CHECK(encoder);
    printf("idle capture rebind encoder: %s\n", md_encoder_get_name(encoder));
    host_capture_sync_init(&ctx.sync, encoder, NULL, &ctx.client_mu,
                            &ctx.pkt_seq, w, h, NULL);
    CaptureThread worker = {.host = &ctx, .capture = &cap};
    pthread_t thread;
    MD_CHECK(pthread_create(&thread, NULL, capture_thread_func, &worker) == 0);
    for (unsigned round = 0; round < 2; round++) {
        MdSessionAccept acceptance = reserve(&session, MD_CAP_VIDEO);
        Pair p = pair(); send_proof(p, &acceptance);
        MD_CHECK(host_bind_candidate(&ctx, p.host) == 0);
        ack(p, MD_CAP_VIDEO);
        pthread_mutex_lock(&ctx.client_mu);
        ctx.client = p.host;
        ctx.pkt_seq = 0;
        host_capture_sync_reset_sequence(&ctx.sync);
        host_request_initial_frame(&ctx);
        pthread_mutex_unlock(&ctx.client_mu);
        pthread_mutex_lock(&idle_mu);
        pthread_cond_signal(&idle_cv);
        pthread_mutex_unlock(&idle_mu);
        MdPacketHeader hdr; uint8_t *payload = NULL;
        MD_CHECK(md_stream_recv(p.peer, &hdr, &payload, 10000) == 0);
        MD_CHECK(hdr.type == MD_PKT_VIDEO_FRAME && hdr.payload_len);
        MdDecoder *decoder = md_decoder_create();
        MD_CHECK(decoder && md_decoder_submit(decoder, payload, hdr.payload_len, 0) == 0);
        unsigned decoded = 0;
        MD_CHECK(md_decoder_poll(decoder, idle_decoded, &decoded) >= 0);
        MD_CHECK(md_decoder_flush(decoder, idle_decoded, &decoded) >= 0);
        MD_CHECK(decoded == 1); /* independently decodable IDR per stream */
        md_decoder_destroy(decoder); free(payload);
        md_stream_close(p.peer);
        payload = NULL;
        MD_CHECK(md_stream_recv(p.host, &hdr, &payload, 1000) < 0); free(payload);
        host_finish_client(&ctx, p.host);
        md_stream_destroy(p.peer);
        OPENSSL_cleanse(acceptance.stream_token, sizeof(acceptance.stream_token));
    }
    atomic_store(&ctx.capture_stopping, true); /* normal worker teardown */
    pthread_mutex_lock(&idle_mu);
    pthread_cond_signal(&idle_cv);
    pthread_mutex_unlock(&idle_mu);
    MD_CHECK(pthread_join(thread, NULL) == 0);
    MD_CHECK(source.pulls == 1 && source.releases == 1);
    free(ctx.cached_frame.data);
    md_encoder_destroy(encoder);
    pthread_mutex_destroy(&ctx.client_mu);
}

static void test_request_cache(void) {
    HostCtx ctx = {0};
    MdSessionRequest req = {0}, first = {0};
    int64_t now = (int64_t)time(NULL);
    for (unsigned i = 0; i < HOST_REQUEST_CACHE_SIZE; i++) {
        MD_CHECK(md_session_request_prepare(&req, now, 30) == 0);
        MD_CHECK(host_remember_request(&ctx, &req, now));
        if (!i) first = req;
    }
    MD_CHECK(!host_remember_request(&ctx, &first, now)); /* A, B, ... A */
    MD_CHECK(md_session_request_prepare(&req, now, 30) == 0);
    MD_CHECK(!host_remember_request(&ctx, &req, now)); /* full fails closed */
    MD_CHECK(!host_remember_request(&ctx, &first, first.expires_at + MD_SESSION_CLOCK_SKEW_S));
    MD_CHECK(host_remember_request(&ctx, &req, first.expires_at + MD_SESSION_CLOCK_SKEW_S + 1));
}

typedef struct {
    HostCtx *ctx;
    MdNostr *nostr;
    MdAgent **agent;
    MdAgentConfig *cfg;
    int result;
} NextSession;
static void *next_session_thread(void *data) {
    NextSession *next = data;
    next->result = host_next_session(next->ctx, next->nostr, true, NULL,
                                     next->agent, next->cfg);
    return NULL;
}

/* Three real negotiations and bound streams, one host/DM subscription.
 * Replay the first request/token after two disconnects, not only the last. */
static void test_relay_rebind(const char *url) {
    MdSession session; md_session_init(&session);
    HostCtx ctx = {.tls_cert_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", .session = &session};
    pthread_mutex_init(&ctx.client_mu, NULL);
    ctx.session_req_ch = go_channel_create(1);
    HistProbe hp, cp; hist_init(&hp, &ctx); hist_init(&cp, NULL);
    MdNostr *host = hist_bridge(url, HIST_HOST_SK, hist_host_dm, &hp);
    ctx.nostr = host;
    MdNostr *client = hist_bridge(url, HIST_CLIENT_SK, hist_client_dm, &cp);
    const char *host_pk = md_nostr_get_npub(host), *client_pk = md_nostr_get_npub(client);
    MdAgentConfig cfg = {.tree_format = MD_TREE_FORMAT_COMPACT};
    MdAgent *agent = NULL;
    MdSessionRequest first = {0}; MdSessionAccept old_accept = {0};
    for (unsigned round = 0; round < 3; round++) {
        NextSession next = {.ctx = &ctx, .nostr = host, .agent = &agent, .cfg = &cfg};
        pthread_t thread;
        MD_CHECK(pthread_create(&thread, NULL, next_session_thread, &next) == 0);
        if (round == 1) {
            /* Membership permits delivery, but current --no-input policy
             * grants none of this request. Refuse it, then keep listening. */
            MdSessionRequest denied = {.capabilities = MD_CAP_INPUT};
            MD_CHECK(md_session_request_prepare(&denied, (int64_t)time(NULL), 30) == 0);
            pthread_mutex_lock(&cp.mu);
            cp.req = denied; cp.armed = true;
            pthread_mutex_unlock(&cp.mu);
            char *bad = md_session_request_to_json(&denied);
            MD_CHECK(bad && md_nostr_send_session_request(client, host_pk, bad) == 0);
            free(bad);
            MD_CHECK(hist_wait(&cp, &cp.rejections, 1));
        }
        MdSessionRequest req = {.capabilities = MD_CAP_VIDEO,
                                .tree_format = round == 1 ? MD_TREE_FORMAT_JSON : MD_TREE_FORMAT_COMPACT};
        MD_CHECK(md_session_request_prepare(&req, (int64_t)time(NULL), 30) == 0);
        if (!round) first = req;
        pthread_mutex_lock(&cp.mu);
        cp.req = req; cp.armed = true;
        pthread_mutex_unlock(&cp.mu);
        char *json = md_session_request_to_json(&req);
        MD_CHECK(json && md_nostr_send_session_request(client, host_pk, json) == 0);
        free(json);
        MD_CHECK(pthread_join(thread, NULL) == 0 && next.result == 0);
        MD_CHECK(hist_wait(&cp, &cp.matched, round + 1));
        MD_CHECK(session.tree_format == req.tree_format && cfg.tree_format == req.tree_format);
        if (!round) old_accept = cp.accepted;
        if (round) {
            MD_CHECK(strcmp(cp.accepted.stream_token, old_accept.stream_token));
            Pair old = pair(); send_proof(old, &old_accept);
            MD_CHECK(host_bind_candidate(&ctx, old.host) < 0);
            closed(old); destroy_pair(old);
            MD_CHECK(session.state == MD_SESSION_NEGOTIATING);
        }
        Pair p = pair(); send_proof(p, &cp.accepted);
        MD_CHECK(host_bind_candidate(&ctx, p.host) == 0);
        MdPacketHeader hdr; uint8_t *payload = NULL;
        MD_CHECK(md_stream_recv(p.peer, &hdr, &payload, 1000) == 0);
        MD_CHECK(hdr.type == MD_PKT_SESSION_INFO);
        free(payload);
        ctx.client = p.host;
        /* Exercise the production capture-to-stream gate on each grant. */
        uint8_t bytes[] = {0, 0, 0, 1, 0x65};
        MdEncodedPacket packet = {.data = bytes, .size = sizeof(bytes)};
        on_encoded(&packet, &ctx);
        payload = NULL;
        MD_CHECK(md_stream_recv(p.peer, &hdr, &payload, 1000) == 0);
        MD_CHECK(hdr.type == MD_PKT_VIDEO_FRAME && hdr.payload_len == sizeof(bytes));
        MD_CHECK(!memcmp(bytes, payload, sizeof(bytes))); free(payload);
        md_stream_close(p.peer);
        payload = NULL;
        MD_CHECK(md_stream_recv(p.host, &hdr, &payload, 1000) < 0); free(payload);
        host_finish_client(&ctx, p.host);
        md_stream_destroy(p.peer);
        MD_CHECK(!ctx.client && !host_has_request(&ctx) && !session.stream_token[0]);
        json = md_session_request_to_json(&first);
        const MdNostrDmInfo live = {.rumor_created_at = first.issued_at, .live = true};
        host_on_dm(client_pk, json, &live, &ctx); free(json);
        MD_CHECK(!host_has_request(&ctx));
    }
    md_agent_destroy(agent);
    md_nostr_destroy(client); md_nostr_destroy(host);
    go_channel_close(ctx.session_req_ch); go_channel_unref(ctx.session_req_ch);
    hist_free(&hp); hist_free(&cp);
    pthread_mutex_destroy(&ctx.client_mu);
    OPENSSL_cleanse(old_accept.stream_token, sizeof(old_accept.stream_token));
}

static void test_agent_replacement_failure(void) {
    MdSession s; md_session_init(&s);
    s.tree_format = MD_TREE_FORMAT_JSON;
    HostCtx ctx = context(&s);
    MdAgentConfig cfg = { .tree_format = MD_TREE_FORMAT_COMPACT };
    MdAgent *agent = md_agent_create(&cfg), *original = agent;
    MD_CHECK(agent);
    ctx.agent = agent;
    fail_agent_create = true;
    MD_CHECK(host_replace_agent(&ctx, &agent, &cfg) < 0);
    MD_CHECK(agent == original && ctx.agent == original);
    fail_agent_create = false;
    MD_CHECK(host_replace_agent(&ctx, &agent, &cfg) == 0);
    MD_CHECK(agent != original && ctx.agent == agent);
    MD_CHECK(cfg.tree_format == MD_TREE_FORMAT_JSON);
    md_agent_destroy(agent);
}

static void test_monitor_switch(void) {
    fake_output_capture=true;
    Pair p=pair(); MdSession s; MdSessionAccept a=reserve(&s,MD_CAP_VIDEO|MD_CAP_MONITOR);
    send_proof(p,&a); MD_CHECK(!md_stream_accept_binding(p.host,&s)); ack(p,s.capabilities);
    HostCtx ctx={.session=&s,.client=p.host,.outputs={.count=2,.switch_supported=true,.selected="id:1"},
        .quality_policy={0,0,1000000,30},.quality={0,0,1000000,30},.output_pending=true,.desired_output="id:2"};
    pthread_mutex_init(&ctx.client_mu,NULL); pthread_cond_init(&ctx.capture_control_cv,NULL);
    MdEncoderConfig cfg={.width=64,.height=64,.fps=30,.bitrate=1000000};
    MdEncoder *e=md_encoder_create(&cfg); MD_CHECK(e);
    MdCaptureRect old={0,0,640,360};
    host_capture_sync_init(&ctx.sync,e,NULL,&ctx.client_mu,&ctx.pkt_seq,64,64,&old);
    host_capture_sync_packet_sent(&ctx.sync,host_capture_sync_frame_tag(&ctx.sync),ctx.pkt_seq++);
    MdCaptureConfig capcfg={.output="id:1"};
    CaptureThread ct={.host=&ctx,.capture=test_capture_create(&capcfg)};
    MdCaptureCtx *old_capture=ct.capture;
    host_switch_output(&ct);
    MD_CHECK(ct.capture!=old_capture && !ctx.output_pending && !strcmp(ctx.outputs.selected,"id:2"));
    MdPacketHeader hdr; uint8_t *payload=NULL;
    MD_CHECK(!md_stream_recv(p.peer,&hdr,&payload,1000) && hdr.type==MD_PKT_SESSION_STATE); free(payload);
    host_encode_frame(&ctx,ct.capture,&ctx.cached_frame,true,true);
    MD_CHECK(ctx.sync.rect.x==640);
    int x,y; MD_CHECK(host_capture_sync_map_point(&ctx.sync,64,64,0,1,1,&x,&y)==HOST_POINT_DROPPED);
    MD_CHECK(!md_stream_recv(p.peer,&hdr,&payload,1000) && hdr.type==MD_PKT_VIDEO_FRAME); free(payload);
    fail_output_capture=true; ctx.output_pending=true; strcpy(ctx.desired_output,"id:1"); ctx.output_revision++;
    old_capture=ct.capture; host_switch_output(&ct);
    MD_CHECK(ct.capture==old_capture && !strcmp(ctx.outputs.selected,"id:2") && !ctx.output_pending);
    MD_CHECK(!md_stream_recv(p.peer,&hdr,&payload,1000) && hdr.type==MD_PKT_SESSION_STATE); free(payload);
    md_capture_destroy(ct.capture); free(ct.owned_output); free(ctx.cached_frame.data); free(ctx.quality_pixels);
    md_encoder_destroy(e); destroy_pair(p); pthread_cond_destroy(&ctx.capture_control_cv); pthread_mutex_destroy(&ctx.client_mu);
    fail_output_capture=fake_output_capture=false;
}
static void test_quality_control(void) {
    Pair p=pair(); MdSession s; MdSessionAccept a=reserve(&s,MD_CAP_VIDEO);
    s.quality_present=true;
    send_proof(p,&a); MD_CHECK(!md_stream_accept_binding(p.host,&s)); ack(p,MD_CAP_VIDEO);
    HostCtx ctx=context(&s); ctx.client=p.host;
    ctx.quality_policy=(MdQuality){1280,720,2000000,30};
    char *json=md_quality_control(&(MdQuality){1920,1080,8000000,60});
    MdPacketHeader h={.type=MD_PKT_SESSION_CONTROL,.payload_len=(uint32_t)strlen(json)};
    handle_client_packet(&ctx,&h,(uint8_t*)json);
    MD_CHECK(ctx.quality.max_width==1280 && ctx.quality.bitrate==2000000 && ctx.quality.fps==30);
    MD_CHECK(ctx.quality_state_pending && md_stream_is_connected(p.host));
    free(json); destroy_pair(p);
    p=pair(); a=reserve(&s,MD_CAP_AGENT); send_proof(p,&a);
    MD_CHECK(!md_stream_accept_binding(p.host,&s)); ack(p,MD_CAP_AGENT);
    ctx=context(&s); ctx.client=p.host;
    handle_client_packet(&ctx,&h,(const uint8_t*)"{}"); closed(p); destroy_pair(p);
}
int main(int argc, char **argv) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    if (argc == 3 && !strcmp(argv[1], "--rebind-fixture")) {
        test_relay_rebind(argv[2]);
        return md_test_report();
    }
    if (argc == 4 && !strcmp(argv[1], "--nip5f-fixture")) {
        hist_nip5f_socket = argv[3];
        hist_session(argv[2], "nip5f-quoted-seal", 0, 0, false);
        return md_test_report();
    }
    if (argc == 4 && !strcmp(argv[1], "--grant-fixture")) {
        test_relay_grant(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
        return md_test_report();
    }
    if (argc == 4 && !strcmp(argv[1], "--two-relay-fixture")) {
        test_two_relay_upgrade(argv[2], argv[3]);
        return md_test_report();
    }
    if (argc == 3 && !strcmp(argv[1], "--history-fixture")) {
        test_relay_history(argv[2]);
        return md_test_report();
    }
    test_monitor_switch();
    test_quality_control();
    test_capture_admission();
    test_idle_capture_rebind();
    test_request_cache();
    test_request_gate();
    test_rearm_request();
    test_agent_replacement_failure();
    test_candidate_and_second_client(); test_unbound_dispatch(); test_bad_proofs();
    test_attended_tls_revocation();
    test_legacy_tls_refusal();
    test_denied_capabilities(); test_late_publication(); test_expiry_and_strict_json();
    return md_test_report();
}
