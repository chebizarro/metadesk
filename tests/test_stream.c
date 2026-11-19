/*
 * metadesk — tests/test_stream.c
 * TCP stream transport tests.
 *
 * Tests server/client connection, packet framing round-trip,
 * and ping/pong latency measurement.
 */
#include "stream.h"
#include "packet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "md_test.h"

/* A broken setup must not cascade into stream operations or thread joins. */
#define REQUIRE(cond) do { \
    int ok = !!(cond); \
    MD_CHECK_MSG(ok, #cond); \
    if (!ok) exit(md_test_report()); \
} while (0)

/* ── Test: server create and destroy ─────────────────────────── */

static int test_server_lifecycle(void) {
    printf("  test_server_lifecycle... ");

    MdStreamServer *srv = md_stream_server_create(NULL, 0);
    REQUIRE(srv != NULL);
    REQUIRE(md_stream_server_port(srv) != 0);

    md_stream_server_destroy(srv);
    printf("OK\n");
    return 0;
}

static int test_bind_addresses(void) {
    printf("  test_bind_addresses... ");
    const char *addresses[] = { "127.0.0.1", "::1", "::ffff:127.0.0.1" };
    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        MdStreamServer *srv = md_stream_server_create(addresses[i], 0);
        REQUIRE(srv != NULL);
        REQUIRE(md_stream_server_port(srv) != 0);
        MdStream *client = md_stream_connect(addresses[i],
                                             md_stream_server_port(srv), 3000);
        REQUIRE(client != NULL);
        MdStream *peer = md_stream_server_accept(srv, 3000);
        REQUIRE(peer != NULL);
        md_stream_destroy(peer);
        md_stream_destroy(client);
        md_stream_server_destroy(srv);
    }
    const char *invalid[] = { "not-an-address", "127.0.0.999", "::gg", "" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        errno = 0;
        MdStreamServer *srv = md_stream_server_create(invalid[i], 0);
        MD_CHECK(srv == NULL);
        MD_CHECK(errno == EINVAL);
        md_stream_server_destroy(srv);
    }
    printf("OK\n");
    return 0;
}

/* ── Test: connect and send/receive ──────────────────────────── */

typedef struct {
    MdStreamServer *srv;
    MdStream *client;
    int done;
} ServerThread;

static void *server_thread(void *arg) {
    ServerThread *st = arg;
    st->client = md_stream_server_accept(st->srv, 5000);
    st->done = 1;
    return NULL;
}

static int test_connect_and_send(void) {
    printf("  test_connect_and_send... ");

    /* Start server */
    MdStreamServer *srv = md_stream_server_create(NULL, 0);
    REQUIRE(srv != NULL);
    REQUIRE(md_stream_server_port(srv) != 0);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    REQUIRE(pthread_create(&tid, NULL, server_thread, &st) == 0);

    /* Connect client */
    MdStream *client = md_stream_connect("127.0.0.1", md_stream_server_port(srv), 3000);
    REQUIRE(client != NULL);
    MD_CHECK(md_stream_is_connected(client));

    /* Wait for server to accept */
    REQUIRE(pthread_join(tid, NULL) == 0);
    REQUIRE(st.client != NULL);
    MD_CHECK(md_stream_is_connected(st.client));

    /* Client sends a video frame packet */
    uint8_t payload[] = "hello from client";
    int ret = md_stream_send(client, MD_PKT_VIDEO_FRAME, 1,
                             payload, sizeof(payload));
    MD_CHECK(ret == 0);

    /* Server receives it */
    MdPacketHeader hdr;
    uint8_t *recv_payload = NULL;
    ret = md_stream_recv(st.client, &hdr, &recv_payload, 3000);
    MD_CHECK(ret == 0);
    MD_CHECK(hdr.type == MD_PKT_VIDEO_FRAME);
    MD_CHECK(hdr.sequence == 1);
    MD_CHECK(hdr.payload_len == sizeof(payload));
    MD_CHECK(memcmp(recv_payload, payload, sizeof(payload)) == 0);
    free(recv_payload);

    /* Server sends back */
    uint8_t reply[] = "hello from server";
    ret = md_stream_send(st.client, MD_PKT_SESSION_INFO, 42,
                         reply, sizeof(reply));
    MD_CHECK(ret == 0);

    /* Client receives */
    recv_payload = NULL;
    ret = md_stream_recv(client, &hdr, &recv_payload, 3000);
    MD_CHECK(ret == 0);
    MD_CHECK(hdr.type == MD_PKT_SESSION_INFO);
    MD_CHECK(hdr.sequence == 42);
    MD_CHECK(memcmp(recv_payload, reply, sizeof(reply)) == 0);
    free(recv_payload);

    /* Verify stats */
    MdStreamStats stats;
    md_stream_get_stats(client, &stats);
    MD_CHECK(stats.packets_sent == 1);
    MD_CHECK(stats.packets_recv == 1);

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);

    printf("OK\n");
    return 0;
}

/* ── Test: ping/pong RTT measurement ─────────────────────────── */

static int test_ping_pong(void) {
    printf("  test_ping_pong... ");

    MdStreamServer *srv = md_stream_server_create(NULL, 0);
    REQUIRE(srv != NULL);
    REQUIRE(md_stream_server_port(srv) != 0);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    REQUIRE(pthread_create(&tid, NULL, server_thread, &st) == 0);

    MdStream *client = md_stream_connect("127.0.0.1", md_stream_server_port(srv), 3000);
    REQUIRE(client != NULL);
    REQUIRE(pthread_join(tid, NULL) == 0);
    REQUIRE(st.client != NULL);

    /* Client sends ping */
    int ret = md_stream_send_ping(client);
    MD_CHECK(ret == 0);

    /* Server receives ping */
    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    ret = md_stream_recv(st.client, &hdr, &payload, 3000);
    MD_CHECK(ret == 0);
    MD_CHECK(hdr.type == MD_PKT_PING);
    free(payload);

    /* Server sends pong */
    ret = md_stream_send(st.client, MD_PKT_PONG, 0, NULL, 0);
    MD_CHECK(ret == 0);

    /* Client receives pong */
    ret = md_stream_recv(client, &hdr, &payload, 3000);
    MD_CHECK(ret == 0);
    MD_CHECK(hdr.type == MD_PKT_PONG);
    free(payload);

    /* Handle pong for RTT */
    md_stream_handle_pong(client, &hdr);

    MdStreamStats stats;
    md_stream_get_stats(client, &stats);
    /* RTT should be very small (localhost) — just verify it's recorded */
    MD_CHECK(stats.last_rtt_ms < 1000);
    MD_CHECK(stats.avg_rtt_ms < 1000);

    printf("OK (rtt=%ums)\n", stats.last_rtt_ms);

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);
    return 0;
}

/* ── Test: empty payload packets ─────────────────────────────── */

static int test_empty_payload(void) {
    printf("  test_empty_payload... ");

    MdStreamServer *srv = md_stream_server_create(NULL, 0);
    REQUIRE(srv != NULL);
    REQUIRE(md_stream_server_port(srv) != 0);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    REQUIRE(pthread_create(&tid, NULL, server_thread, &st) == 0);

    MdStream *client = md_stream_connect("127.0.0.1", md_stream_server_port(srv), 3000);
    REQUIRE(client != NULL);
    REQUIRE(pthread_join(tid, NULL) == 0);
    REQUIRE(st.client != NULL);

    /* Send packet with no payload */
    int ret = md_stream_send(client, MD_PKT_PING, 99, NULL, 0);
    MD_CHECK(ret == 0);

    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    ret = md_stream_recv(st.client, &hdr, &payload, 3000);
    MD_CHECK(ret == 0);
    MD_CHECK(hdr.type == MD_PKT_PING);
    MD_CHECK(hdr.payload_len == 0);
    MD_CHECK(payload == NULL);

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);

    printf("OK\n");
    return 0;
}

/* ── Test: recv timeout ──────────────────────────────────────── */

static int test_recv_timeout(void) {
    printf("  test_recv_timeout... ");

    MdStreamServer *srv = md_stream_server_create(NULL, 0);
    REQUIRE(srv != NULL);
    REQUIRE(md_stream_server_port(srv) != 0);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    REQUIRE(pthread_create(&tid, NULL, server_thread, &st) == 0);

    MdStream *client = md_stream_connect("127.0.0.1", md_stream_server_port(srv), 3000);
    REQUIRE(client != NULL);
    REQUIRE(pthread_join(tid, NULL) == 0);

    REQUIRE(st.client != NULL);

    /* Try to recv with a short timeout — should return 1 (timeout) */
    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    int ret = md_stream_recv(client, &hdr, &payload, 100);
    MD_CHECK(ret == 1);

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);

    printf("OK\n");
    return 0;
}

/* ── Test: TLS server/client round-trip ───────────────────────── */

static int test_tls_roundtrip(void) {
    printf("  test_tls_roundtrip... ");

    /* Create TLS server with ephemeral self-signed cert */
    MdStreamTlsConfig tls_cfg = { .enabled = true };
    MdStreamServer *srv = md_stream_server_create_tls(NULL, 0, &tls_cfg);
    REQUIRE(srv != NULL);
    REQUIRE(md_stream_server_port(srv) != 0);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    REQUIRE(pthread_create(&tid, NULL, server_thread, &st) == 0);

    /* TLS client (no peer verification for self-signed) */
    MdStreamTlsConfig client_tls = { .enabled = true, .verify_peer = false };
    MdStream *client = md_stream_connect_tls("127.0.0.1", md_stream_server_port(srv),
                                             5000, &client_tls);
    REQUIRE(client != NULL);
    MD_CHECK(md_stream_is_tls(client));

    REQUIRE(pthread_join(tid, NULL) == 0);
    REQUIRE(st.client != NULL);
    MD_CHECK(md_stream_is_tls(st.client));

    /* Send a packet through TLS and verify */
    const char *msg = "hello-tls";
    int ret = md_stream_send(client, MD_PKT_SESSION_INFO, 1,
                             (const uint8_t *)msg, (uint32_t)strlen(msg));
    MD_CHECK(ret == 0);

    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    ret = md_stream_recv(st.client, &hdr, &payload, 5000);
    MD_CHECK(ret == 0);
    MD_CHECK(hdr.type == MD_PKT_SESSION_INFO);
    MD_CHECK(hdr.payload_len == strlen(msg));
    MD_CHECK(payload != NULL);
    MD_CHECK(memcmp(payload, msg, strlen(msg)) == 0);
    free(payload);

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);

    printf("OK\n");
    return 0;
}

/* ── Test: plaintext stream reports no TLS ───────────────────── */

static int test_plaintext_no_tls(void) {
    printf("  test_plaintext_no_tls... ");

    MdStreamServer *srv = md_stream_server_create(NULL, 0);
    REQUIRE(srv != NULL);
    REQUIRE(md_stream_server_port(srv) != 0);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    REQUIRE(pthread_create(&tid, NULL, server_thread, &st) == 0);

    MdStream *client = md_stream_connect("127.0.0.1", md_stream_server_port(srv), 5000);
    REQUIRE(client != NULL);
    MD_CHECK(!md_stream_is_tls(client));

    REQUIRE(pthread_join(tid, NULL) == 0);
    REQUIRE(st.client != NULL);
    MD_CHECK(!md_stream_is_tls(st.client));

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);

    printf("OK\n");
    return 0;
}

/* ── Test: concurrent send + recv on one stream, peer closes ── */

/* md-9sa6: the host sends video from its capture thread while the main
 * thread receives, polls md_stream_is_connected and reads stats on the same
 * stream. Either direction may clear `connected` when the peer goes away.
 * Under TSan (-Db_sanitize=thread) this reported a data race on connected
 * and on the traffic counters before they became atomics. */

#define CONCURRENT_PAYLOAD 4096

typedef struct {
    MdStream *s;
    uint32_t sends_ok;
} Sender;

static void *sender_main(void *arg) {
    Sender *x = arg;
    static const uint8_t frame[CONCURRENT_PAYLOAD];
    uint32_t seq = 0;
    while (md_stream_send(x->s, MD_PKT_VIDEO_FRAME, seq++,
                          frame, sizeof(frame)) == 0)
        x->sends_ok++;
    return NULL;
}

typedef struct {
    MdStream *s;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool client_got_packet;      /* set by the client's receive loop */
} Peer;

/* Takes a few frames, answers with pings, and goes away only once the
 * client's receive loop has taken one: by then that loop has read stats and
 * received while the sender was still streaming (nothing fails a send
 * before this close). */
static void *peer_main(void *arg) {
    Peer *p = arg;
    for (int i = 0; i < 4; i++) {
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        MD_CHECK(md_stream_recv(p->s, &hdr, &payload, 5000) == 0);
        MD_CHECK(hdr.type == MD_PKT_VIDEO_FRAME);
        free(payload);
    }
    for (uint32_t i = 0; i < 3; i++)
        MD_CHECK(md_stream_send(p->s, MD_PKT_PING, i, NULL, 0) == 0);
    const uint64_t until = md_monotonic_ns() + 10000000000ull;
    pthread_mutex_lock(&p->mu);
    while (!p->client_got_packet &&
           md_cond_timedwait_until(&p->cv, &p->mu, until) != ETIMEDOUT) {}
    pthread_mutex_unlock(&p->mu);
    md_stream_destroy(p->s);
    return NULL;
}

static int test_concurrent_send_recv_peer_close(void) {
    printf("  test_concurrent_send_recv_peer_close... ");

    MdStreamServer *srv = md_stream_server_create(NULL, 0);
    REQUIRE(srv != NULL);
    ServerThread st = { .srv = srv };
    pthread_t tid;
    REQUIRE(pthread_create(&tid, NULL, server_thread, &st) == 0);
    MdStream *client = md_stream_connect("127.0.0.1", md_stream_server_port(srv), 3000);
    REQUIRE(client != NULL);
    REQUIRE(pthread_join(tid, NULL) == 0);
    REQUIRE(st.client != NULL);
    Peer peer = { .s = st.client };
    REQUIRE(pthread_mutex_init(&peer.mu, NULL) == 0);
    REQUIRE(md_cond_init_monotonic(&peer.cv) == 0);

    Sender x = { .s = client };
    pthread_t sender, peer_tid;
    REQUIRE(pthread_create(&sender, NULL, sender_main, &x) == 0);
    REQUIRE(pthread_create(&peer_tid, NULL, peer_main, &peer) == 0);

    /* The host's receive loop, on the stream the sender is using. */
    const int64_t deadline = md_now_ms() + 10000;
    bool got_packet = false;
    while (md_stream_is_connected(client) && md_now_ms() < deadline) {
        MdStreamStats stats;
        md_stream_get_stats(client, &stats);
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        if (md_stream_recv(client, &hdr, &payload, 10) != 0) continue;
        free(payload);
        if (!got_packet) {
            got_packet = true;
            pthread_mutex_lock(&peer.mu);
            peer.client_got_packet = true;
            pthread_cond_signal(&peer.cv);
            pthread_mutex_unlock(&peer.mu);
        }
    }
    /* The peer's close cleared connected from one side or the other. */
    MD_CHECK(!md_stream_is_connected(client));
    /* On a timeout, shutdown() still wakes a sender blocked in send(). */
    md_stream_close(client);
    REQUIRE(pthread_join(sender, NULL) == 0);
    REQUIRE(pthread_join(peer_tid, NULL) == 0);

    MdStreamStats stats;
    md_stream_get_stats(client, &stats);
    MD_CHECK(got_packet);
    MD_CHECK(stats.packets_recv >= 1);
    MD_CHECK(stats.bytes_recv == (uint64_t)stats.packets_recv * MD_PACKET_HEADER_SIZE);
    MD_CHECK(x.sends_ok >= 4);
    MD_CHECK(stats.packets_sent == x.sends_ok);
    MD_CHECK(stats.bytes_sent ==
             (uint64_t)x.sends_ok * (MD_PACKET_HEADER_SIZE + CONCURRENT_PAYLOAD));

    pthread_cond_destroy(&peer.cv);
    pthread_mutex_destroy(&peer.mu);
    md_stream_destroy(client);
    md_stream_server_destroy(srv);
    printf("OK\n");
    return 0;
}

/* ── Main ───────────────────────────────────────────────────── */

int main(void) {
    /* Ignore SIGPIPE — normal for socket tests where peers disconnect */
    signal(SIGPIPE, SIG_IGN);

    printf("test_stream: TCP stream transport tests\n");

    int failures = 0;
    failures += test_server_lifecycle();
    failures += test_bind_addresses();
    failures += test_connect_and_send();
    failures += test_ping_pong();
    failures += test_empty_payload();
    failures += test_recv_timeout();
    failures += test_tls_roundtrip();
    failures += test_plaintext_no_tls();
    failures += test_concurrent_send_recv_peer_close();

    md_test_failures += failures;
    return md_test_report();
}
