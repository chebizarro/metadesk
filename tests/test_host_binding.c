/* Private loopback tests of real host admission/capture/dispatch. No desktop. */
#define main md_host_main_not_run
#define md_agent_send_tree test_send_tree
#define md_agent_handle_action test_action
#define md_input_mouse_move test_move
#define md_nostr_send_session_accept test_publish
#include "../src/host/main.c"
#undef main
#include "md_test.h"

static unsigned tree_calls, action_calls, input_calls, publish_calls;
static MdSessionAccept published;
int test_publish(MdNostr *n, const char *peer, const char *json) {
    (void)n; (void)peer;
    publish_calls++;
    MD_CHECK(md_session_accept_from_json(json, &published) == 0);
    return 0;
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
int test_move(MdInput *i, int x, int y) {
    (void)i; (void)x; (void)y; input_calls++; return 0;
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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    MdSessionAccept acc;
    MD_CHECK(md_session_prepare_binding(s, &acc,
        (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000) == 0);
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
static HostCtx context(MdSession *s) {
    HostCtx ctx = { .session = s, .agent = (MdAgent *)(uintptr_t)1,
                    .input = (MdInput *)(uintptr_t)1 };
    return ctx;
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
    uint8_t move[MD_INPUT_EVENT_HDR_LEN];
    MD_CHECK(md_input_event_pack(move, sizeof(move), MD_INPUT_EV_MOVE, 0, 1, 2, NULL, 0));
    MdPacketHeader allowed = { .type = MD_PKT_INPUT, .payload_len = sizeof(move) };
    unsigned moves = input_calls, actions = action_calls;
    handle_client_packet(&ctx, &allowed, move);
    allowed.type = MD_PKT_ACTION;
    handle_client_packet(&ctx, &allowed, move);
    MD_CHECK(input_calls == moves + 1 && action_calls == actions + 1);
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
        uint8_t input[MD_INPUT_EVENT_HDR_LEN];
        MD_CHECK(md_input_event_pack(input, sizeof(input), MD_INPUT_EV_MOVE, 0, 1, 2, NULL, 0));
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
        uint8_t input[MD_INPUT_EVENT_HDR_LEN];
        MD_CHECK(md_input_event_pack(input, sizeof(input), MD_INPUT_EV_MOVE, 0, 12, 34, NULL, 0));
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
    MD_CHECK(host_publish_session_accept(&ctx, NULL) == 0);
    MD_CHECK(publish_calls == before + 1 && s.stream_token[0]);
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
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        MdSessionAccept acc;
        MD_CHECK(md_session_prepare_binding(&s, &acc,
            (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000) == 0);
        Pair p = pair(); send_proof(p, &acc);
        MD_CHECK(host_bind_candidate(&ctx, p.host) == 0);
        ack(p, MD_CAP_VIDEO); ctx.client = p.host;
        unsigned trees = tree_calls; host_send_initial_tree(&ctx);
        MD_CHECK(tree_calls == trees);
        uint8_t input[MD_INPUT_EVENT_HDR_LEN];
        MD_CHECK(md_input_event_pack(input, sizeof(input), MD_INPUT_EV_MOVE, 0, 1, 2, NULL, 0));
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
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 4 && !strcmp(argv[1], "--grant-fixture")) {
        test_relay_grant(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
        return md_test_report();
    }
    test_candidate_and_second_client(); test_unbound_dispatch(); test_bad_proofs();
    test_denied_capabilities(); test_late_publication(); test_expiry_and_strict_json();
    return md_test_report();
}
