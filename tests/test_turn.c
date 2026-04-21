/*
 * metadesk — test_turn.c
 * TURN relay client tests (RFC 5766 packet building/parsing).
 *
 * Tests low-level packet construction and parsing without requiring
 * a live TURN server.
 */
#include "turn.h"
#include "stun.h"  /* MD_STUN_MAGIC_COOKIE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

/* ── Helpers ─────────────────────────────────────────────────── */

static void fill_txn_id(uint8_t txn[12], uint8_t val) {
    memset(txn, val, 12);
}

/* ── ChannelData framing tests ───────────────────────────────── */

static void test_channel_data_build(void) {
    uint8_t buf[128];
    const char *payload = "Hello TURN";
    size_t plen = strlen(payload);

    int n = md_turn_build_channel_data(buf, sizeof(buf),
                                       0x4000, payload, plen);
    assert(n > 0);

    /* Header: 2 channel + 2 length */
    assert(buf[0] == 0x40 && buf[1] == 0x00); /* channel 0x4000 */
    uint16_t len = (uint16_t)(buf[2] << 8 | buf[3]);
    assert(len == plen);

    /* Payload */
    assert(memcmp(buf + 4, payload, plen) == 0);

    /* Total size: header(4) + padded payload */
    size_t padded = (plen + 3) & ~(size_t)3;
    assert(n == (int)(4 + padded));

    printf("  PASS: channel data build\n");
}

static void test_channel_data_parse(void) {
    uint8_t buf[128];
    const char *payload = "Test data";
    size_t plen = strlen(payload);

    int n = md_turn_build_channel_data(buf, sizeof(buf),
                                       0x4001, payload, plen);
    assert(n > 0);

    uint16_t channel, length;
    int rc = md_turn_parse_channel_data(buf, (size_t)n, &channel, &length);
    assert(rc == 0);
    assert(channel == 0x4001);
    assert(length == plen);
    assert(memcmp(buf + 4, payload, plen) == 0);

    printf("  PASS: channel data parse\n");
}

static void test_channel_data_roundtrip(void) {
    /* Build → parse → verify payload integrity */
    const char *msg = "Roundtrip test message with some padding bytes";
    size_t msg_len = strlen(msg);
    uint8_t frame[256];

    int flen = md_turn_build_channel_data(frame, sizeof(frame),
                                          0x5ABC, msg, msg_len);
    assert(flen > 0);

    uint16_t ch, len;
    assert(md_turn_parse_channel_data(frame, flen, &ch, &len) == 0);
    assert(ch == 0x5ABC);
    assert(len == msg_len);
    assert(memcmp(frame + 4, msg, msg_len) == 0);

    printf("  PASS: channel data roundtrip\n");
}

static void test_channel_data_min_channel(void) {
    uint8_t buf[32];
    uint8_t data = 0x42;
    int n = md_turn_build_channel_data(buf, sizeof(buf),
                                       MD_TURN_CHANNEL_MIN, &data, 1);
    assert(n > 0); /* 0x4000 is valid */

    printf("  PASS: channel data min channel\n");
}

static void test_channel_data_max_channel(void) {
    uint8_t buf[32];
    uint8_t data = 0x42;
    int n = md_turn_build_channel_data(buf, sizeof(buf),
                                       MD_TURN_CHANNEL_MAX, &data, 1);
    assert(n > 0); /* 0x7FFE is valid */

    printf("  PASS: channel data max channel\n");
}

static void test_channel_data_invalid_channel(void) {
    uint8_t buf[32];
    uint8_t data = 0x42;

    /* Below minimum */
    assert(md_turn_build_channel_data(buf, sizeof(buf),
                                      0x3FFF, &data, 1) == -1);
    /* Above maximum */
    assert(md_turn_build_channel_data(buf, sizeof(buf),
                                      0x7FFF, &data, 1) == -1);
    /* Zero channel */
    assert(md_turn_build_channel_data(buf, sizeof(buf),
                                      0x0000, &data, 1) == -1);

    printf("  PASS: channel data invalid channel rejected\n");
}

static void test_channel_data_empty_payload(void) {
    uint8_t buf[32];
    /* Empty payload should be rejected */
    assert(md_turn_build_channel_data(buf, sizeof(buf),
                                      0x4000, "x", 0) == -1);
    assert(md_turn_build_channel_data(buf, sizeof(buf),
                                      0x4000, NULL, 5) == -1);

    printf("  PASS: channel data empty payload rejected\n");
}

static void test_channel_data_parse_short(void) {
    uint8_t buf[2] = {0x40, 0x00};
    uint16_t ch, len;
    /* Too short for header */
    assert(md_turn_parse_channel_data(buf, 2, &ch, &len) == -1);
    /* NULL inputs */
    assert(md_turn_parse_channel_data(NULL, 4, &ch, &len) == -1);
    assert(md_turn_parse_channel_data(buf, 4, NULL, &len) == -1);

    printf("  PASS: channel data parse rejects short/null\n");
}

/* ── Allocate Request tests ──────────────────────────────────── */

static void test_allocate_build_no_auth(void) {
    uint8_t txn[12];
    fill_txn_id(txn, 0xAA);

    uint8_t buf[256];
    int n = md_turn_build_allocate(buf, sizeof(buf), txn,
                                   NULL, NULL, NULL, NULL);
    assert(n > 0);

    /* STUN header */
    uint16_t type = (uint16_t)(buf[0] << 8 | buf[1]);
    assert(type == MD_TURN_ALLOCATE_REQUEST);

    uint32_t cookie = (uint32_t)(buf[4] << 24 | buf[5] << 16 |
                                  buf[6] << 8 | buf[7]);
    assert(cookie == MD_STUN_MAGIC_COOKIE);

    /* Transaction ID */
    assert(memcmp(buf + 8, txn, 12) == 0);

    /* Should have REQUESTED-TRANSPORT attribute */
    /* Minimal: header(20) + attr(4+4) = 28 bytes */
    assert(n >= 28);

    printf("  PASS: allocate build no auth\n");
}

static void test_allocate_build_with_auth(void) {
    uint8_t txn[12];
    fill_txn_id(txn, 0xBB);

    uint8_t buf[512];
    int n = md_turn_build_allocate(buf, sizeof(buf), txn,
                                   "testuser", "testrealm",
                                   "abc123nonce", "password");
    assert(n > 0);

    /* Should be larger than no-auth version (has USERNAME, REALM, NONCE) */
    assert(n > 28);

    /* Still a valid STUN header */
    uint16_t type = (uint16_t)(buf[0] << 8 | buf[1]);
    assert(type == MD_TURN_ALLOCATE_REQUEST);

    printf("  PASS: allocate build with auth\n");
}

static void test_allocate_build_null_checks(void) {
    uint8_t txn[12];
    fill_txn_id(txn, 0x11);

    /* NULL buf */
    assert(md_turn_build_allocate(NULL, 256, txn,
                                  NULL, NULL, NULL, NULL) == -1);
    /* Buffer too small */
    uint8_t small[10];
    assert(md_turn_build_allocate(small, sizeof(small), txn,
                                  NULL, NULL, NULL, NULL) == -1);
    /* NULL txn */
    uint8_t buf[256];
    assert(md_turn_build_allocate(buf, sizeof(buf), NULL,
                                  NULL, NULL, NULL, NULL) == -1);

    printf("  PASS: allocate build null checks\n");
}

/* ── Allocate Response parse tests ───────────────────────────── */

/* Helper: build a fake allocate success response */
static int build_fake_alloc_response(uint8_t *buf, size_t buf_len,
                                     const uint8_t txn[12],
                                     const char *relay_ip,
                                     uint16_t relay_port,
                                     uint32_t lifetime) {
    if (buf_len < 100) return -1;

    /* Attributes */
    uint8_t attrs[128];
    int attr_len = 0;

    /* XOR-RELAYED-ADDRESS (IPv4) */
    struct in_addr addr;
    inet_pton(AF_INET, relay_ip, &addr);

    uint8_t relay_val[8];
    relay_val[0] = 0;      /* reserved */
    relay_val[1] = 0x01;   /* IPv4 */
    uint16_t xport = relay_port ^ (uint16_t)(MD_STUN_MAGIC_COOKIE >> 16);
    relay_val[2] = (uint8_t)(xport >> 8);
    relay_val[3] = (uint8_t)(xport);
    uint32_t xaddr = ntohl(addr.s_addr) ^ MD_STUN_MAGIC_COOKIE;
    relay_val[4] = (uint8_t)(xaddr >> 24);
    relay_val[5] = (uint8_t)(xaddr >> 16);
    relay_val[6] = (uint8_t)(xaddr >> 8);
    relay_val[7] = (uint8_t)(xaddr);

    /* Attr header: type(2) + len(2) + value(8) = 12 */
    attrs[attr_len++] = 0x00; attrs[attr_len++] = 0x16; /* XOR-RELAYED-ADDR */
    attrs[attr_len++] = 0x00; attrs[attr_len++] = 0x08; /* length = 8 */
    memcpy(attrs + attr_len, relay_val, 8); attr_len += 8;

    /* LIFETIME attribute */
    attrs[attr_len++] = 0x00; attrs[attr_len++] = 0x0D; /* LIFETIME */
    attrs[attr_len++] = 0x00; attrs[attr_len++] = 0x04; /* length = 4 */
    attrs[attr_len++] = (uint8_t)(lifetime >> 24);
    attrs[attr_len++] = (uint8_t)(lifetime >> 16);
    attrs[attr_len++] = (uint8_t)(lifetime >> 8);
    attrs[attr_len++] = (uint8_t)(lifetime);

    /* STUN header */
    buf[0] = 0x01; buf[1] = 0x03; /* Allocate Success Response */
    buf[2] = (uint8_t)(attr_len >> 8);
    buf[3] = (uint8_t)(attr_len);
    buf[4] = 0x21; buf[5] = 0x12; buf[6] = 0xA4; buf[7] = 0x42; /* magic */
    memcpy(buf + 8, txn, 12);
    memcpy(buf + 20, attrs, attr_len);

    return 20 + attr_len;
}

static void test_allocate_response_parse(void) {
    uint8_t txn[12];
    fill_txn_id(txn, 0xCC);

    uint8_t resp[256];
    int resp_len = build_fake_alloc_response(resp, sizeof(resp), txn,
                                             "198.51.100.42", 49152, 600);
    assert(resp_len > 0);

    char relay_ip[64] = {0};
    uint16_t relay_port = 0;
    uint32_t lifetime = 0;

    int rc = md_turn_parse_allocate_response(resp, resp_len, txn,
                                             relay_ip, sizeof(relay_ip),
                                             &relay_port, &lifetime);
    assert(rc == 0);
    assert(strcmp(relay_ip, "198.51.100.42") == 0);
    assert(relay_port == 49152);
    assert(lifetime == 600);

    printf("  PASS: allocate response parse\n");
}

static void test_allocate_response_wrong_txn(void) {
    uint8_t txn[12], wrong_txn[12];
    fill_txn_id(txn, 0xDD);
    fill_txn_id(wrong_txn, 0xEE);

    uint8_t resp[256];
    int resp_len = build_fake_alloc_response(resp, sizeof(resp), txn,
                                             "10.0.0.1", 5000, 300);
    assert(resp_len > 0);

    char ip[64]; uint16_t port; uint32_t lt;
    /* Wrong txn should fail */
    assert(md_turn_parse_allocate_response(resp, resp_len, wrong_txn,
                                           ip, sizeof(ip), &port, &lt) == -1);

    printf("  PASS: allocate response wrong txn rejected\n");
}

static void test_allocate_response_null_checks(void) {
    uint8_t txn[12]; fill_txn_id(txn, 0x11);
    char ip[64]; uint16_t port; uint32_t lt;

    assert(md_turn_parse_allocate_response(NULL, 20, txn, ip, 64, &port, &lt) == -1);
    uint8_t buf[4] = {0};
    assert(md_turn_parse_allocate_response(buf, 4, txn, ip, 64, &port, &lt) == -1);

    printf("  PASS: allocate response null/short rejected\n");
}

/* ── Error response parse tests ──────────────────────────────── */

/* Helper: build a fake 401 error response */
static int build_fake_error_response(uint8_t *buf, size_t buf_len,
                                     const uint8_t txn[12],
                                     int error_code,
                                     const char *nonce,
                                     const char *realm) {
    if (buf_len < 100) return -1;

    uint8_t attrs[256];
    int attr_len = 0;

    /* ERROR-CODE: class in byte 2, number in byte 3 */
    int cls = error_code / 100;
    int num = error_code % 100;
    uint8_t err_val[4] = { 0, 0, (uint8_t)cls, (uint8_t)num };
    attrs[attr_len++] = 0x00; attrs[attr_len++] = 0x09; /* ERROR-CODE */
    attrs[attr_len++] = 0x00; attrs[attr_len++] = 0x04;
    memcpy(attrs + attr_len, err_val, 4); attr_len += 4;

    /* NONCE */
    if (nonce) {
        size_t nlen = strlen(nonce);
        size_t padded = (nlen + 3) & ~(size_t)3;
        attrs[attr_len++] = 0x00; attrs[attr_len++] = 0x15; /* NONCE */
        attrs[attr_len++] = (uint8_t)(nlen >> 8);
        attrs[attr_len++] = (uint8_t)(nlen);
        memcpy(attrs + attr_len, nonce, nlen);
        if (padded > nlen) memset(attrs + attr_len + nlen, 0, padded - nlen);
        attr_len += (int)padded;
    }

    /* REALM */
    if (realm) {
        size_t rlen = strlen(realm);
        size_t padded = (rlen + 3) & ~(size_t)3;
        attrs[attr_len++] = 0x00; attrs[attr_len++] = 0x14; /* REALM */
        attrs[attr_len++] = (uint8_t)(rlen >> 8);
        attrs[attr_len++] = (uint8_t)(rlen);
        memcpy(attrs + attr_len, realm, rlen);
        if (padded > rlen) memset(attrs + attr_len + rlen, 0, padded - rlen);
        attr_len += (int)padded;
    }

    /* STUN header: Allocate Error Response = 0x0113 */
    buf[0] = 0x01; buf[1] = 0x13;
    buf[2] = (uint8_t)(attr_len >> 8);
    buf[3] = (uint8_t)(attr_len);
    buf[4] = 0x21; buf[5] = 0x12; buf[6] = 0xA4; buf[7] = 0x42;
    memcpy(buf + 8, txn, 12);
    memcpy(buf + 20, attrs, attr_len);

    return 20 + attr_len;
}

static void test_error_parse_401(void) {
    uint8_t txn[12];
    fill_txn_id(txn, 0x55);

    uint8_t resp[256];
    int resp_len = build_fake_error_response(resp, sizeof(resp), txn,
                                             401, "abc123", "sharegap.net");
    assert(resp_len > 0);

    char nonce[256] = {0};
    char realm[128] = {0};

    int err = md_turn_parse_error(resp, resp_len, txn,
                                  nonce, sizeof(nonce),
                                  realm, sizeof(realm));
    assert(err == 401);
    assert(strcmp(nonce, "abc123") == 0);
    assert(strcmp(realm, "sharegap.net") == 0);

    printf("  PASS: error parse 401 with nonce/realm\n");
}

static void test_error_parse_438(void) {
    uint8_t txn[12];
    fill_txn_id(txn, 0x66);

    uint8_t resp[256];
    int resp_len = build_fake_error_response(resp, sizeof(resp), txn,
                                             438, "new_nonce", "realm2");
    assert(resp_len > 0);

    char nonce[256] = {0};
    char realm[128] = {0};

    int err = md_turn_parse_error(resp, resp_len, txn,
                                  nonce, sizeof(nonce),
                                  realm, sizeof(realm));
    assert(err == 438);
    assert(strcmp(nonce, "new_nonce") == 0);

    printf("  PASS: error parse 438 stale nonce\n");
}

static void test_error_parse_wrong_txn(void) {
    uint8_t txn[12], wrong[12];
    fill_txn_id(txn, 0x77);
    fill_txn_id(wrong, 0x88);

    uint8_t resp[256];
    int resp_len = build_fake_error_response(resp, sizeof(resp), txn,
                                             401, "n", "r");
    assert(resp_len > 0);

    assert(md_turn_parse_error(resp, resp_len, wrong,
                               NULL, 0, NULL, 0) == -1);

    printf("  PASS: error parse wrong txn rejected\n");
}

/* ── ChannelBind request tests ───────────────────────────────── */

static void test_channel_bind_build(void) {
    uint8_t txn[12];
    fill_txn_id(txn, 0x99);

    uint8_t buf[512];
    int n = md_turn_build_channel_bind(buf, sizeof(buf), txn,
                                       0x4000, "192.0.2.1", 5000,
                                       "user", "realm", "nonce", "pass");
    assert(n > 0);

    /* Verify STUN header type */
    uint16_t type = (uint16_t)(buf[0] << 8 | buf[1]);
    assert(type == MD_TURN_CHANBIND_REQUEST);

    /* Verify cookie and txn */
    uint32_t cookie = (uint32_t)(buf[4] << 24 | buf[5] << 16 |
                                  buf[6] << 8 | buf[7]);
    assert(cookie == MD_STUN_MAGIC_COOKIE);
    assert(memcmp(buf + 8, txn, 12) == 0);

    printf("  PASS: channel bind build\n");
}

static void test_channel_bind_invalid_channel(void) {
    uint8_t txn[12];
    fill_txn_id(txn, 0xAA);
    uint8_t buf[512];

    /* Below minimum */
    assert(md_turn_build_channel_bind(buf, sizeof(buf), txn,
                                      0x3FFF, "1.2.3.4", 80,
                                      NULL, NULL, NULL, NULL) == -1);

    /* Above maximum */
    assert(md_turn_build_channel_bind(buf, sizeof(buf), txn,
                                      0x7FFF, "1.2.3.4", 80,
                                      NULL, NULL, NULL, NULL) == -1);

    printf("  PASS: channel bind invalid channel rejected\n");
}

static void test_channel_bind_no_auth(void) {
    uint8_t txn[12];
    fill_txn_id(txn, 0xBB);
    uint8_t buf[512];

    int n = md_turn_build_channel_bind(buf, sizeof(buf), txn,
                                       0x4001, "10.0.0.5", 9000,
                                       NULL, NULL, NULL, NULL);
    assert(n > 0);
    /* Should be smaller without auth attrs */
    assert(n < 100);

    printf("  PASS: channel bind no auth\n");
}

/* ── Alloc state tests (no server) ───────────────────────────── */

static void test_turn_close_null(void) {
    /* Should not crash */
    md_turn_close(NULL);

    MdTurnAlloc alloc = {0};
    alloc.fd = -1;
    md_turn_close(&alloc);

    printf("  PASS: turn close null safety\n");
}

static void test_turn_send_not_bound(void) {
    MdTurnAlloc alloc = {0};
    alloc.fd = 999;  /* fake */
    alloc.channel_bound = false;

    /* Should fail — channel not bound */
    assert(md_turn_send(&alloc, "test", 4) == -1);

    printf("  PASS: turn send not bound rejected\n");
}

static void test_turn_recv_bad_fd(void) {
    MdTurnAlloc alloc = {0};
    alloc.fd = -1;

    char buf[64];
    assert(md_turn_recv(&alloc, buf, sizeof(buf), 100) == -1);

    printf("  PASS: turn recv bad fd rejected\n");
}

static void test_turn_allocate_null(void) {
    MdTurnAlloc alloc;
    assert(md_turn_allocate(NULL, &alloc) == -1);

    MdTurnConfig cfg = {0};
    assert(md_turn_allocate(&cfg, NULL) == -1);

    /* Empty server should fail */
    assert(md_turn_allocate(&cfg, &alloc) == -1);

    printf("  PASS: turn allocate null/empty rejected\n");
}

static void test_turn_refresh_not_allocated(void) {
    MdTurnAlloc alloc = {0};
    alloc.fd = -1;
    alloc.allocated = false;

    assert(md_turn_refresh(&alloc) == -1);

    printf("  PASS: turn refresh not allocated rejected\n");
}

/* ── Constants verification ──────────────────────────────────── */

static void test_constants(void) {
    /* RFC 5766 channel range */
    assert(MD_TURN_CHANNEL_MIN == 0x4000);
    assert(MD_TURN_CHANNEL_MAX == 0x7FFE);

    /* STUN/TURN message types */
    assert(MD_TURN_ALLOCATE_REQUEST == 0x0003);
    assert(MD_TURN_ALLOCATE_RESPONSE == 0x0103);
    assert(MD_TURN_CHANBIND_REQUEST == 0x0009);

    /* Default port */
    assert(MD_TURN_DEFAULT_PORT == 3478);

    printf("  PASS: RFC 5766 constants\n");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_turn:\n");

    /* ChannelData framing (8 tests) */
    test_channel_data_build();
    test_channel_data_parse();
    test_channel_data_roundtrip();
    test_channel_data_min_channel();
    test_channel_data_max_channel();
    test_channel_data_invalid_channel();
    test_channel_data_empty_payload();
    test_channel_data_parse_short();

    /* Allocate request building (3 tests) */
    test_allocate_build_no_auth();
    test_allocate_build_with_auth();
    test_allocate_build_null_checks();

    /* Allocate response parsing (3 tests) */
    test_allocate_response_parse();
    test_allocate_response_wrong_txn();
    test_allocate_response_null_checks();

    /* Error response parsing (3 tests) */
    test_error_parse_401();
    test_error_parse_438();
    test_error_parse_wrong_txn();

    /* ChannelBind building (3 tests) */
    test_channel_bind_build();
    test_channel_bind_invalid_channel();
    test_channel_bind_no_auth();

    /* State/lifecycle safety (5 tests) */
    test_turn_close_null();
    test_turn_send_not_bound();
    test_turn_recv_bad_fd();
    test_turn_allocate_null();
    test_turn_refresh_not_allocated();

    /* Constants (1 test) */
    test_constants();

    printf("All TURN tests passed.\n");
    return 0;
}
