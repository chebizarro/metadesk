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

#ifdef __linux__
#include <dlfcn.h>
#include <stdatomic.h>
/* md-z6nw: OpenSSL forbids two threads inside one SSL object at once, and
 * neither TSan nor ASan can see that inside the uninstrumented libssl. So
 * on Linux, where libmetadesk.so's SSL calls bind to these definitions, the
 * test counts the threads inside each SSL object itself. */
#define SSL_CALL_SLOTS 16
static struct { const void *ssl; int inside; } ssl_calls[SSL_CALL_SLOTS];
static pthread_mutex_t ssl_calls_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int ssl_overlaps;

static void ssl_enter(const void *ssl, int delta) {
    pthread_mutex_lock(&ssl_calls_mu);
    int slot = -1;
    for (int i = 0; i < SSL_CALL_SLOTS && slot < 0; i++)
        if (ssl_calls[i].ssl == ssl) slot = i;
    for (int i = 0; i < SSL_CALL_SLOTS && slot < 0; i++)
        if (ssl_calls[i].inside == 0) slot = i;   /* reuse an idle slot */
    if (slot >= 0) {
        ssl_calls[slot].ssl = ssl;
        if (delta > 0 && ssl_calls[slot].inside > 0)
            atomic_fetch_add(&ssl_overlaps, 1);
        ssl_calls[slot].inside += delta;
    }
    pthread_mutex_unlock(&ssl_calls_mu);
}

static void *ssl_real(const char *name) {
    void *fn = dlsym(RTLD_NEXT, name);
    if (!fn) abort();
    return fn;
}

/* The real function, resolved once per call site without a racy static. */
#define SSL_CALL(rtype, name, params, args) \
    rtype name params { \
        static _Atomic(rtype (*) params) real; \
        rtype (*fn) params = atomic_load(&real); \
        if (!fn) { fn = (rtype (*) params)ssl_real(#name); atomic_store(&real, fn); } \
        ssl_enter(ssl, 1); \
        rtype r = fn args; \
        ssl_enter(ssl, -1); \
        return r; \
    }
SSL_CALL(int, SSL_read, (SSL *ssl, void *buf, int num), (ssl, buf, num))
SSL_CALL(int, SSL_write, (SSL *ssl, const void *buf, int num), (ssl, buf, num))
SSL_CALL(int, SSL_get_error, (const SSL *ssl, int rc), (ssl, rc))
SSL_CALL(int, SSL_pending, (const SSL *ssl), (ssl))
#define SSL_CALLS_COUNTED 1
#else
#define SSL_CALLS_COUNTED 0
static atomic_int ssl_overlaps; /* stays 0: no interposition here */
#endif

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

/* A connected pair: *client dialled, *peer accepted; TLS uses the server's
 * ephemeral self-signed certificate, unverified. */
static MdStreamServer *connect_pair(bool tls, MdStream **client, MdStream **peer) {
    MdStreamTlsConfig srv_tls = { .enabled = true };
    MdStreamServer *srv = md_stream_server_create_tls(NULL, 0, tls ? &srv_tls : NULL);
    REQUIRE(srv != NULL);
    ServerThread st = { .srv = srv };
    pthread_t tid;
    REQUIRE(pthread_create(&tid, NULL, server_thread, &st) == 0);
    MdStreamTlsConfig cli_tls = { .enabled = true, .verify_peer = false };
    *client = tls ? md_stream_connect_tls("127.0.0.1", md_stream_server_port(srv),
                                          3000, &cli_tls)
                  : md_stream_connect("127.0.0.1", md_stream_server_port(srv), 3000);
    REQUIRE(*client != NULL);
    REQUIRE(pthread_join(tid, NULL) == 0);
    REQUIRE(st.client != NULL);
    REQUIRE(md_stream_is_tls(*client) == tls && md_stream_is_tls(st.client) == tls);
    *peer = st.client;
    return srv;
}

/* md-z6nw: on TLS both directions drive one SSL object. */
static int test_concurrent_send_recv_peer_close(bool tls) {
    printf("  test_concurrent_send_recv_peer_close%s... ", tls ? " (TLS)" : "");

    MdStream *client = NULL, *accepted = NULL;
    MdStreamServer *srv = connect_pair(tls, &client, &accepted);
    ServerThread st = { .srv = srv, .client = accepted };
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
    MD_CHECK(atomic_load(&ssl_overlaps) == 0); /* never two threads in one SSL */

    pthread_cond_destroy(&peer.cv);
    pthread_mutex_destroy(&peer.mu);
    md_stream_destroy(client);
    md_stream_server_destroy(srv);
    printf("OK\n");
    return 0;
}

/* ── Test: TLS full duplex on both ends at once (md-z6nw) ────── */

/* Each end sends from one thread while another receives, as the host does.
 * Packets are big enough that every sender fills its socket buffer and
 * waits on WANT_WRITE while the receivers decrypt. The client only starts
 * sending once it holds half the server's packets, so for that first half
 * the server's receiver waits in md_stream_recv with nothing to read while
 * its sender streams: a lock held across that wait would starve the sender
 * and leave both ends stuck until the receive timeout. */

#define DUPLEX_PACKETS 48
#define DUPLEX_MAX_LEN (192 * 1024)

static uint32_t duplex_len(uint32_t seq) {
    return 1 + (seq * 40503u) % DUPLEX_MAX_LEN;
}

static uint8_t duplex_byte(uint8_t tag, uint32_t seq, uint32_t i) {
    return (uint8_t)(tag * 131u + seq * 31u + i * 7u);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool open;
} Gate;

typedef struct {
    MdStream *s;
    uint8_t tag;          /* mixed into every byte this direction carries */
    Gate *gate;           /* tx: wait for it; rx: open it at half way */
    uint32_t done;        /* packets sent or received intact */
    uint64_t bytes;
} Duplex;

static void gate_open(Gate *g) {
    pthread_mutex_lock(&g->mu);
    g->open = true;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

static void *duplex_tx(void *arg) {
    Duplex *d = arg;
    if (d->gate) {
        const uint64_t until = md_monotonic_ns() + 20000000000ull;
        pthread_mutex_lock(&d->gate->mu);
        while (!d->gate->open &&
               md_cond_timedwait_until(&d->gate->cv, &d->gate->mu, until) != ETIMEDOUT) {}
        const bool open = d->gate->open;
        pthread_mutex_unlock(&d->gate->mu);
        if (!open) return NULL;
    }
    uint8_t *buf = malloc(DUPLEX_MAX_LEN);
    if (!buf) return NULL;
    for (uint32_t seq = 0; seq < DUPLEX_PACKETS; seq++) {
        const uint32_t len = duplex_len(seq);
        for (uint32_t i = 0; i < len; i++) buf[i] = duplex_byte(d->tag, seq, i);
        if (md_stream_send(d->s, MD_PKT_VIDEO_FRAME, seq, buf, len) != 0) break;
        d->done++;
        d->bytes += MD_PACKET_HEADER_SIZE + len;
    }
    free(buf);
    return NULL;
}

static void *duplex_rx(void *arg) {
    Duplex *d = arg;
    for (uint32_t seq = 0; seq < DUPLEX_PACKETS; seq++) {
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        if (md_stream_recv(d->s, &hdr, &payload, 20000) != 0) break;
        bool intact = hdr.type == MD_PKT_VIDEO_FRAME && hdr.sequence == seq &&
                      hdr.payload_len == duplex_len(seq) && payload;
        for (uint32_t i = 0; intact && i < hdr.payload_len; i++)
            intact = payload[i] == duplex_byte(d->tag, seq, i);
        free(payload);
        if (!intact) break;
        d->done++;
        d->bytes += MD_PACKET_HEADER_SIZE + hdr.payload_len;
        if (d->gate && d->done == DUPLEX_PACKETS / 2) gate_open(d->gate);
    }
    if (d->gate) gate_open(d->gate); /* never strand the sender */
    return NULL;
}

static int test_tls_concurrent_duplex(void) {
    printf("  test_tls_concurrent_duplex... ");
    MdStream *client = NULL, *server = NULL;
    MdStreamServer *srv = connect_pair(true, &client, &server);

    Gate gate = { .open = false };
    REQUIRE(pthread_mutex_init(&gate.mu, NULL) == 0);
    REQUIRE(md_cond_init_monotonic(&gate.cv) == 0);
    Duplex srv_rx = { .s = server, .tag = 2 };
    Duplex srv_tx = { .s = server, .tag = 1 };
    Duplex cli_rx = { .s = client, .tag = 1, .gate = &gate };
    Duplex cli_tx = { .s = client, .tag = 2, .gate = &gate };
    Duplex *ends[] = { &srv_rx, &cli_rx, &srv_tx, &cli_tx };
    void *(*mains[])(void *) = { duplex_rx, duplex_rx, duplex_tx, duplex_tx };
    pthread_t tids[4];
    const uint64_t start = md_monotonic_ns();
    for (int i = 0; i < 4; i++)
        REQUIRE(pthread_create(&tids[i], NULL, mains[i], ends[i]) == 0);
    for (int i = 0; i < 4; i++)
        REQUIRE(pthread_join(tids[i], NULL) == 0);
    const uint64_t elapsed_ms = (md_monotonic_ns() - start) / 1000000u;

    for (int i = 0; i < 4; i++)
        MD_CHECK(ends[i]->done == DUPLEX_PACKETS);
    MD_CHECK(srv_tx.bytes == cli_rx.bytes && cli_tx.bytes == srv_rx.bytes);
    /* No direction waited out a 20 s receive timeout. */
    MD_CHECK(elapsed_ms < 20000);
    MdStreamStats ss, cs;
    md_stream_get_stats(server, &ss);
    md_stream_get_stats(client, &cs);
    MD_CHECK(ss.packets_sent == DUPLEX_PACKETS && ss.packets_recv == DUPLEX_PACKETS);
    MD_CHECK(cs.packets_sent == DUPLEX_PACKETS && cs.packets_recv == DUPLEX_PACKETS);
    MD_CHECK(ss.bytes_sent == srv_tx.bytes && ss.bytes_recv == srv_rx.bytes);
    MD_CHECK(cs.bytes_sent == cli_tx.bytes && cs.bytes_recv == cli_rx.bytes);
    MD_CHECK(md_stream_is_connected(server) && md_stream_is_connected(client));
    MD_CHECK(atomic_load(&ssl_overlaps) == 0); /* never two threads in one SSL */

    pthread_cond_destroy(&gate.cv);
    pthread_mutex_destroy(&gate.mu);
    md_stream_destroy(client);
    md_stream_destroy(server);
    md_stream_server_destroy(srv);
    printf("OK (%llu ms%s)\n", (unsigned long long)elapsed_ms,
           SSL_CALLS_COUNTED ? ", SSL calls counted" : "");
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
    failures += test_concurrent_send_recv_peer_close(false);
    failures += test_concurrent_send_recv_peer_close(true);
    failures += test_tls_concurrent_duplex();

    md_test_failures += failures;
    return md_test_report();
}
