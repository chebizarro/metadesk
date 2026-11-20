/* Private loopback tests of real host admission/capture/dispatch. No desktop. */
#define main md_host_main_not_run
#define md_agent_send_tree test_send_tree
#define md_agent_handle_action test_action
#define md_input_mouse_move test_move
#define md_nostr_send_session_accept test_publish
#include "../src/host/main.c"
#undef main
#undef md_nostr_send_session_accept
#include "md_test.h"
/* nostr.h's prototype was renamed by the macro above; restore the real one. */
int md_nostr_send_session_accept(MdNostr *n, const char *client_pubkey_hex,
                                 const char *json_payload);

static unsigned tree_calls, action_calls, input_calls, publish_calls;
static MdSessionAccept published;
/* Records the acceptance; with a real bridge (relay fixture) also publishes. */
int test_publish(MdNostr *n, const char *peer, const char *json) {
    publish_calls++;
    MD_CHECK(md_session_accept_from_json(json, &published) == 0);
    return n ? md_nostr_send_session_accept(n, peer, json) : 0;
}
int md_host_account_cli(int argc, char **argv) { (void)argc; (void)argv; return -1; }
int md_host_access_cli(int argc, char **argv) { (void)argc; (void)argv; return -1; }
int test_send_tree(MdAgent *a, MdStream *s, uint32_t *seq) {
    (void)a; (void)s; (void)seq; tree_calls++; return 0;
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
    HostCtx ctx = { .session = s, .agent = (MdAgent *)(uintptr_t)1,
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
    struct pollfd fd = { .fd = md_stream_get_fd(first.peer), .events = POLLIN };
    MD_CHECK(poll(&fd, 1, 0) == 0);
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
    MD_CHECK(write(md_stream_get_fd(wrong.peer), header, sizeof(header)) == sizeof(header));
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
        struct pollfd fd = { .fd = md_stream_get_fd(p.peer), .events = POLLIN };
        MD_CHECK(poll(&fd, 1, 0) == 0);
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

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    HostCtx *host;               /* host probe: forward to the real gate */
    MdSessionRequest req;        /* client probe: its current request */
    bool armed;
    MdSessionAccept accepted;
    unsigned matched, ignored_history, ignored_live, host_history, host_live;
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
    (void)pk;
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
    MdNostrConfig cfg = { .sk_hex = sk, .relay_urls = urls, .relay_count = count,
                          .allowlist_open = true };
    MdNostrCallbacks cbs = { .on_dm = cb, .dm_userdata = p };
    MdNostr *n = md_nostr_create(&cfg, &cbs);
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
    MD_CHECK(!ctx.session_requested && hp.host_live == 0);

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
    MD_CHECK(!ctx.session_requested && hp.host_live == 0);

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

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
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
    test_request_gate();
    test_candidate_and_second_client(); test_unbound_dispatch(); test_bad_proofs();
    test_denied_capabilities(); test_late_publication(); test_expiry_and_strict_json();
    return md_test_report();
}
