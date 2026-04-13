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
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

/* Use a high ephemeral port to avoid conflicts */
#define TEST_PORT 17700

/* ── Test: server create and destroy ─────────────────────────── */

static int test_server_lifecycle(void) {
    printf("  test_server_lifecycle... ");

    MdStreamServer *srv = md_stream_server_create(NULL, TEST_PORT);
    assert(srv != NULL);

    md_stream_server_destroy(srv);
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
    MdStreamServer *srv = md_stream_server_create(NULL, TEST_PORT + 1);
    assert(srv != NULL);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &st);

    /* Give server a moment to start accepting */
    usleep(50000);

    /* Connect client */
    MdStream *client = md_stream_connect("127.0.0.1", TEST_PORT + 1, 3000);
    assert(client != NULL);
    assert(md_stream_is_connected(client));

    /* Wait for server to accept */
    pthread_join(tid, NULL);
    assert(st.client != NULL);
    assert(md_stream_is_connected(st.client));

    /* Client sends a video frame packet */
    uint8_t payload[] = "hello from client";
    int ret = md_stream_send(client, MD_PKT_VIDEO_FRAME, 1,
                             payload, sizeof(payload));
    assert(ret == 0);

    /* Server receives it */
    MdPacketHeader hdr;
    uint8_t *recv_payload = NULL;
    ret = md_stream_recv(st.client, &hdr, &recv_payload, 3000);
    assert(ret == 0);
    assert(hdr.type == MD_PKT_VIDEO_FRAME);
    assert(hdr.sequence == 1);
    assert(hdr.payload_len == sizeof(payload));
    assert(memcmp(recv_payload, payload, sizeof(payload)) == 0);
    free(recv_payload);

    /* Server sends back */
    uint8_t reply[] = "hello from server";
    ret = md_stream_send(st.client, MD_PKT_SESSION_INFO, 42,
                         reply, sizeof(reply));
    assert(ret == 0);

    /* Client receives */
    recv_payload = NULL;
    ret = md_stream_recv(client, &hdr, &recv_payload, 3000);
    assert(ret == 0);
    assert(hdr.type == MD_PKT_SESSION_INFO);
    assert(hdr.sequence == 42);
    assert(memcmp(recv_payload, reply, sizeof(reply)) == 0);
    free(recv_payload);

    /* Verify stats */
    MdStreamStats stats;
    md_stream_get_stats(client, &stats);
    assert(stats.packets_sent == 1);
    assert(stats.packets_recv == 1);

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);

    printf("OK\n");
    return 0;
}

/* ── Test: ping/pong RTT measurement ─────────────────────────── */

static int test_ping_pong(void) {
    printf("  test_ping_pong... ");

    MdStreamServer *srv = md_stream_server_create(NULL, TEST_PORT + 2);
    assert(srv != NULL);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &st);
    usleep(50000);

    MdStream *client = md_stream_connect("127.0.0.1", TEST_PORT + 2, 3000);
    assert(client != NULL);
    pthread_join(tid, NULL);
    assert(st.client != NULL);

    /* Client sends ping */
    int ret = md_stream_send_ping(client);
    assert(ret == 0);

    /* Server receives ping */
    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    ret = md_stream_recv(st.client, &hdr, &payload, 3000);
    assert(ret == 0);
    assert(hdr.type == MD_PKT_PING);
    free(payload);

    /* Server sends pong */
    ret = md_stream_send(st.client, MD_PKT_PONG, 0, NULL, 0);
    assert(ret == 0);

    /* Client receives pong */
    ret = md_stream_recv(client, &hdr, &payload, 3000);
    assert(ret == 0);
    assert(hdr.type == MD_PKT_PONG);
    free(payload);

    /* Handle pong for RTT */
    md_stream_handle_pong(client, &hdr);

    MdStreamStats stats;
    md_stream_get_stats(client, &stats);
    /* RTT should be very small (localhost) — just verify it's recorded */
    assert(stats.last_rtt_ms < 1000);
    assert(stats.avg_rtt_ms < 1000);

    printf("OK (rtt=%ums)\n", stats.last_rtt_ms);

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);
    return 0;
}

/* ── Test: empty payload packets ─────────────────────────────── */

static int test_empty_payload(void) {
    printf("  test_empty_payload... ");

    MdStreamServer *srv = md_stream_server_create(NULL, TEST_PORT + 3);
    assert(srv != NULL);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &st);
    usleep(50000);

    MdStream *client = md_stream_connect("127.0.0.1", TEST_PORT + 3, 3000);
    assert(client != NULL);
    pthread_join(tid, NULL);
    assert(st.client != NULL);

    /* Send packet with no payload */
    int ret = md_stream_send(client, MD_PKT_PING, 99, NULL, 0);
    assert(ret == 0);

    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    ret = md_stream_recv(st.client, &hdr, &payload, 3000);
    assert(ret == 0);
    assert(hdr.type == MD_PKT_PING);
    assert(hdr.payload_len == 0);
    assert(payload == NULL);

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);

    printf("OK\n");
    return 0;
}

/* ── Test: recv timeout ──────────────────────────────────────── */

static int test_recv_timeout(void) {
    printf("  test_recv_timeout... ");

    MdStreamServer *srv = md_stream_server_create(NULL, TEST_PORT + 4);
    assert(srv != NULL);

    ServerThread st = { .srv = srv };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &st);
    usleep(50000);

    MdStream *client = md_stream_connect("127.0.0.1", TEST_PORT + 4, 3000);
    assert(client != NULL);
    pthread_join(tid, NULL);

    /* Try to recv with a short timeout — should return 1 (timeout) */
    MdPacketHeader hdr;
    uint8_t *payload = NULL;
    int ret = md_stream_recv(client, &hdr, &payload, 100);
    assert(ret == 1);

    md_stream_destroy(client);
    md_stream_destroy(st.client);
    md_stream_server_destroy(srv);

    printf("OK\n");
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_stream: TCP stream transport tests\n");

    int failures = 0;
    failures += test_server_lifecycle();
    failures += test_connect_and_send();
    failures += test_ping_pong();
    failures += test_empty_payload();
    failures += test_recv_timeout();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures;
}
