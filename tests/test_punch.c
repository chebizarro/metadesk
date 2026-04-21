/*
 * metadesk — test_punch.c
 * UDP hole punch coordinator tests.
 *
 * Tests packet build/parse logic and a full punch handshake on
 * loopback using two threads (simulating two peers).
 */
#include "../src/fips-nat/punch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>

/* ── Packet build/parse tests ────────────────────────────────── */

static void test_build_probe(void) {
    uint8_t session[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t buf[32];

    int len = md_punch_build_packet(buf, sizeof(buf),
                                    MD_PUNCH_TYPE_PROBE, session);
    assert(len == MD_PUNCH_PACKET_SIZE);

    /* Check magic "MDPH" = 0x4D445048 */
    assert(buf[0] == 0x4D && buf[1] == 0x44 &&
           buf[2] == 0x50 && buf[3] == 0x48);

    /* Type = PROBE */
    assert(buf[4] == MD_PUNCH_TYPE_PROBE);

    /* Version */
    assert(buf[5] == MD_PUNCH_VERSION);

    /* Session ID at offset 8 */
    assert(memcmp(buf + 8, session, 16) == 0);

    printf("  PASS: build PROBE packet\n");
}

static void test_build_ack(void) {
    uint8_t session[16] = {0};
    uint8_t buf[32];

    int len = md_punch_build_packet(buf, sizeof(buf),
                                    MD_PUNCH_TYPE_ACK, session);
    assert(len == MD_PUNCH_PACKET_SIZE);
    assert(buf[4] == MD_PUNCH_TYPE_ACK);

    printf("  PASS: build ACK packet\n");
}

static void test_build_null_safety(void) {
    uint8_t session[16] = {0};
    uint8_t buf[32];

    assert(md_punch_build_packet(NULL, 32, MD_PUNCH_TYPE_PROBE, session) == -1);
    assert(md_punch_build_packet(buf, 32, MD_PUNCH_TYPE_PROBE, NULL) == -1);

    /* Buffer too small */
    assert(md_punch_build_packet(buf, 10, MD_PUNCH_TYPE_PROBE, session) == -1);

    /* Invalid type */
    assert(md_punch_build_packet(buf, 32, 0xFF, session) == -1);
    assert(md_punch_build_packet(buf, 32, 0x00, session) == -1);

    printf("  PASS: build null safety\n");
}

static void test_parse_probe(void) {
    uint8_t session[16] = {0xAA,0xBB,0xCC,0xDD,0,0,0,0,0,0,0,0,0,0,0,0};
    uint8_t buf[32];

    md_punch_build_packet(buf, sizeof(buf), MD_PUNCH_TYPE_PROBE, session);

    uint8_t type = 0;
    int ret = md_punch_parse_packet(buf, MD_PUNCH_PACKET_SIZE, session, &type);
    assert(ret == 0);
    assert(type == MD_PUNCH_TYPE_PROBE);

    printf("  PASS: parse PROBE packet\n");
}

static void test_parse_ack(void) {
    uint8_t session[16] = {0};
    uint8_t buf[32];

    md_punch_build_packet(buf, sizeof(buf), MD_PUNCH_TYPE_ACK, session);

    uint8_t type = 0;
    int ret = md_punch_parse_packet(buf, MD_PUNCH_PACKET_SIZE, session, &type);
    assert(ret == 0);
    assert(type == MD_PUNCH_TYPE_ACK);

    printf("  PASS: parse ACK packet\n");
}

static void test_parse_wrong_magic(void) {
    uint8_t session[16] = {0};
    uint8_t buf[32];

    md_punch_build_packet(buf, sizeof(buf), MD_PUNCH_TYPE_PROBE, session);
    buf[0] = 0xFF; /* corrupt magic */

    uint8_t type;
    assert(md_punch_parse_packet(buf, MD_PUNCH_PACKET_SIZE, session, &type) == -1);

    printf("  PASS: parse rejects wrong magic\n");
}

static void test_parse_wrong_version(void) {
    uint8_t session[16] = {0};
    uint8_t buf[32];

    md_punch_build_packet(buf, sizeof(buf), MD_PUNCH_TYPE_PROBE, session);
    buf[5] = 99; /* wrong version */

    uint8_t type;
    assert(md_punch_parse_packet(buf, MD_PUNCH_PACKET_SIZE, session, &type) == -1);

    printf("  PASS: parse rejects wrong version\n");
}

static void test_parse_wrong_session(void) {
    uint8_t session[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t wrong[16]   = {0};
    uint8_t buf[32];

    md_punch_build_packet(buf, sizeof(buf), MD_PUNCH_TYPE_PROBE, session);

    uint8_t type;
    assert(md_punch_parse_packet(buf, MD_PUNCH_PACKET_SIZE, wrong, &type) == -1);

    printf("  PASS: parse rejects wrong session ID\n");
}

static void test_parse_truncated(void) {
    uint8_t session[16] = {0};
    uint8_t buf[32];

    md_punch_build_packet(buf, sizeof(buf), MD_PUNCH_TYPE_PROBE, session);

    uint8_t type;
    /* Too short */
    assert(md_punch_parse_packet(buf, 10, session, &type) == -1);
    assert(md_punch_parse_packet(buf, 0, session, &type) == -1);

    printf("  PASS: parse rejects truncated packet\n");
}

static void test_parse_null_safety(void) {
    uint8_t session[16] = {0};
    uint8_t buf[32] = {0};
    uint8_t type;

    assert(md_punch_parse_packet(NULL, 24, session, &type) == -1);
    assert(md_punch_parse_packet(buf, 24, NULL, &type) == -1);
    assert(md_punch_parse_packet(buf, 24, session, NULL) == -1);

    printf("  PASS: parse null safety\n");
}

static void test_roundtrip_probe(void) {
    uint8_t session[16];
    for (int i = 0; i < 16; i++) session[i] = (uint8_t)(i * 17);

    uint8_t buf[32];
    int len = md_punch_build_packet(buf, sizeof(buf),
                                    MD_PUNCH_TYPE_PROBE, session);
    assert(len == MD_PUNCH_PACKET_SIZE);

    uint8_t type;
    int ret = md_punch_parse_packet(buf, (size_t)len, session, &type);
    assert(ret == 0);
    assert(type == MD_PUNCH_TYPE_PROBE);

    printf("  PASS: probe roundtrip\n");
}

static void test_roundtrip_ack(void) {
    uint8_t session[16];
    for (int i = 0; i < 16; i++) session[i] = (uint8_t)(0xFF - i);

    uint8_t buf[32];
    int len = md_punch_build_packet(buf, sizeof(buf),
                                    MD_PUNCH_TYPE_ACK, session);
    assert(len == MD_PUNCH_PACKET_SIZE);

    uint8_t type;
    int ret = md_punch_parse_packet(buf, (size_t)len, session, &type);
    assert(ret == 0);
    assert(type == MD_PUNCH_TYPE_ACK);

    printf("  PASS: ack roundtrip\n");
}

/* ── Loopback hole punch test (two threads) ──────────────────── */

typedef struct {
    MdPunchConfig config;
    MdPunchResult result;
    int           ret;
} PunchThreadArgs;

static void *punch_thread(void *arg) {
    PunchThreadArgs *a = arg;
    a->ret = md_punch_execute(&a->config, &a->result);
    return NULL;
}

static void test_loopback_punch(void) {
    /* Two peers punching each other on loopback.
     * This tests the full state machine: PROBING → GOT_PROBE → CONFIRMED */

    uint8_t session[16] = {0xDE,0xAD,0xBE,0xEF,
                            0xCA,0xFE,0xBA,0xBE,
                            0x12,0x34,0x56,0x78,
                            0x9A,0xBC,0xDE,0xF0};

    /* Peer A: binds a specific port, punches to peer B's port */
    PunchThreadArgs peer_a;
    memset(&peer_a, 0, sizeof(peer_a));
    strncpy(peer_a.config.peer_ip, "127.0.0.1", sizeof(peer_a.config.peer_ip));
    peer_a.config.peer_port        = 19801;
    peer_a.config.local_bind_port  = 19800;
    peer_a.config.timeout_ms       = 3000;
    peer_a.config.probe_interval_ms = 50;
    memcpy(peer_a.config.session_id, session, 16);

    /* Peer B: binds the port A is targeting, punches to A's port */
    PunchThreadArgs peer_b;
    memset(&peer_b, 0, sizeof(peer_b));
    strncpy(peer_b.config.peer_ip, "127.0.0.1", sizeof(peer_b.config.peer_ip));
    peer_b.config.peer_port        = 19800;
    peer_b.config.local_bind_port  = 19801;
    peer_b.config.timeout_ms       = 3000;
    peer_b.config.probe_interval_ms = 50;
    memcpy(peer_b.config.session_id, session, 16);

    pthread_t ta, tb;
    int rc_a = pthread_create(&ta, NULL, punch_thread, &peer_a);
    int rc_b = pthread_create(&tb, NULL, punch_thread, &peer_b);
    assert(rc_a == 0 && rc_b == 0);

    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    assert(peer_a.ret == 0);
    assert(peer_b.ret == 0);
    assert(peer_a.result.fd >= 0);
    assert(peer_b.result.fd >= 0);
    assert(peer_a.result.peer_port == 19801);
    assert(peer_b.result.peer_port == 19800);

    /* Verify the sockets work: send data through the punched path */
    const char *msg_a = "hello from A";
    const char *msg_b = "hello from B";
    char buf[64];

    ssize_t sent = send(peer_a.result.fd, msg_a, strlen(msg_a), 0);
    assert(sent == (ssize_t)strlen(msg_a));

    ssize_t recvd = recv(peer_b.result.fd, buf, sizeof(buf), 0);
    assert(recvd == (ssize_t)strlen(msg_a));
    assert(memcmp(buf, msg_a, strlen(msg_a)) == 0);

    sent = send(peer_b.result.fd, msg_b, strlen(msg_b), 0);
    assert(sent == (ssize_t)strlen(msg_b));

    recvd = recv(peer_a.result.fd, buf, sizeof(buf), 0);
    assert(recvd == (ssize_t)strlen(msg_b));
    assert(memcmp(buf, msg_b, strlen(msg_b)) == 0);

    close(peer_a.result.fd);
    close(peer_b.result.fd);

    printf("  PASS: loopback hole punch (two threads, bidirectional data)\n");
}

static void test_execute_null_safety(void) {
    MdPunchResult result;
    MdPunchConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    assert(md_punch_execute(NULL, &result) == -1);
    assert(md_punch_execute(&cfg, NULL) == -1);

    /* Empty peer IP */
    assert(md_punch_execute(&cfg, &result) == -1);

    /* Zero peer port */
    strncpy(cfg.peer_ip, "127.0.0.1", sizeof(cfg.peer_ip));
    cfg.peer_port = 0;
    assert(md_punch_execute(&cfg, &result) == -1);

    printf("  PASS: execute null safety\n");
}

static void test_punch_peer_null_safety(void) {
    assert(md_punch_peer(NULL, 1234) == -1);
    assert(md_punch_peer("127.0.0.1", 0) == -1);
    assert(md_punch_peer("127.0.0.1", -1) == -1);
    assert(md_punch_peer("127.0.0.1", 70000) == -1);

    printf("  PASS: punch_peer null safety\n");
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    printf("test_punch:\n");

    /* Packet build/parse tests */
    test_build_probe();
    test_build_ack();
    test_build_null_safety();
    test_parse_probe();
    test_parse_ack();
    test_parse_wrong_magic();
    test_parse_wrong_version();
    test_parse_wrong_session();
    test_parse_truncated();
    test_parse_null_safety();
    test_roundtrip_probe();
    test_roundtrip_ack();

    /* Integration tests */
    test_execute_null_safety();
    test_punch_peer_null_safety();
    test_loopback_punch();

    printf("test_punch: ALL %d TESTS PASSED\n", 15);
    return 0;
}
