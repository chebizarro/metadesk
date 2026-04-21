/*
 * metadesk — test_stun.c
 * STUN Binding Request/Response parsing tests.
 *
 * Tests the build and parse functions directly with crafted byte
 * buffers — no network I/O required. Verifies:
 *   - Request building (header, magic cookie, transaction ID)
 *   - IPv4 XOR-MAPPED-ADDRESS parsing
 *   - IPv6 XOR-MAPPED-ADDRESS parsing
 *   - MAPPED-ADDRESS fallback (no XOR)
 *   - XOR-MAPPED-ADDRESS preferred over MAPPED-ADDRESS
 *   - Error handling (truncation, wrong type, bad cookie, txn mismatch)
 *   - Server address parsing (host:port, [IPv6]:port, plain host)
 */
#include "../src/fips-nat/stun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

/* ── Byte helpers (matching stun.c) ──────────────────────────── */

static inline void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

/* ── Helper: build a minimal STUN Binding Response ───────────── */

/*
 * Build a STUN response with a single attribute into buf.
 * Returns total message length.
 */
static int build_response(uint8_t *buf, size_t buf_len,
                          const uint8_t txn_id[12],
                          uint16_t attr_type,
                          const uint8_t *attr_val, uint16_t attr_val_len) {
    /* Attribute: 4-byte header + value, padded to 4-byte boundary */
    uint16_t attr_padded = (attr_val_len + 3) & ~3u;
    uint16_t msg_len = 4 + attr_padded; /* attr header + padded value */
    size_t total = MD_STUN_HEADER_SIZE + msg_len;

    if (total > buf_len) return -1;

    memset(buf, 0, total);

    /* Header */
    put_u16(buf + 0, MD_STUN_BINDING_RESPONSE);
    put_u16(buf + 2, msg_len);
    put_u32(buf + 4, MD_STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txn_id, 12);

    /* Attribute */
    uint8_t *attr = buf + MD_STUN_HEADER_SIZE;
    put_u16(attr + 0, attr_type);
    put_u16(attr + 2, attr_val_len);
    memcpy(attr + 4, attr_val, attr_val_len);

    return (int)total;
}

/* ── Helper: build XOR-MAPPED-ADDRESS value for IPv4 ─────────── */

static int build_xor_mapped_ipv4(uint8_t *val, size_t val_len,
                                  const uint8_t txn_id[12],
                                  const char *ip_str, uint16_t port) {
    (void)txn_id;
    if (val_len < 8) return -1;

    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) return -1;

    uint8_t addr_bytes[4];
    memcpy(addr_bytes, &addr, 4);

    /* XOR port with high 16 bits of magic cookie */
    uint16_t xport = port ^ (uint16_t)(MD_STUN_MAGIC_COOKIE >> 16);

    /* XOR address with magic cookie */
    uint32_t cookie = MD_STUN_MAGIC_COOKIE;
    uint8_t xaddr[4];
    xaddr[0] = addr_bytes[0] ^ (uint8_t)(cookie >> 24);
    xaddr[1] = addr_bytes[1] ^ (uint8_t)(cookie >> 16);
    xaddr[2] = addr_bytes[2] ^ (uint8_t)(cookie >>  8);
    xaddr[3] = addr_bytes[3] ^ (uint8_t)(cookie);

    val[0] = 0;                       /* reserved */
    val[1] = MD_STUN_FAMILY_IPV4;     /* family   */
    put_u16(val + 2, xport);
    memcpy(val + 4, xaddr, 4);

    return 8;
}

/* ── Helper: build XOR-MAPPED-ADDRESS value for IPv6 ─────────── */

static int build_xor_mapped_ipv6(uint8_t *val, size_t val_len,
                                  const uint8_t txn_id[12],
                                  const char *ip_str, uint16_t port) {
    if (val_len < 20) return -1;

    struct in6_addr addr;
    if (inet_pton(AF_INET6, ip_str, &addr) != 1) return -1;

    uint8_t addr_bytes[16];
    memcpy(addr_bytes, &addr, 16);

    /* XOR port with high 16 bits of magic cookie */
    uint16_t xport = port ^ (uint16_t)(MD_STUN_MAGIC_COOKIE >> 16);

    /* XOR address with magic cookie || transaction ID */
    uint8_t xor_key[16];
    put_u32(xor_key, MD_STUN_MAGIC_COOKIE);
    memcpy(xor_key + 4, txn_id, 12);

    uint8_t xaddr[16];
    for (int i = 0; i < 16; i++)
        xaddr[i] = addr_bytes[i] ^ xor_key[i];

    val[0] = 0;                       /* reserved */
    val[1] = MD_STUN_FAMILY_IPV6;     /* family   */
    put_u16(val + 2, xport);
    memcpy(val + 4, xaddr, 16);

    return 20;
}

/* ── Helper: build MAPPED-ADDRESS value (no XOR) for IPv4 ────── */

static int build_mapped_ipv4(uint8_t *val, size_t val_len,
                              const char *ip_str, uint16_t port) {
    if (val_len < 8) return -1;

    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) return -1;

    val[0] = 0;                       /* reserved */
    val[1] = MD_STUN_FAMILY_IPV4;     /* family   */
    put_u16(val + 2, port);
    memcpy(val + 4, &addr, 4);

    return 8;
}

/* ── Tests ───────────────────────────────────────────────────── */

static void test_build_request(void) {
    uint8_t txn_id[12] = {1,2,3,4,5,6,7,8,9,10,11,12};
    uint8_t buf[64];

    int len = md_stun_build_request(buf, sizeof(buf), txn_id);
    assert(len == MD_STUN_HEADER_SIZE);

    /* Check message type: Binding Request */
    assert(buf[0] == 0x00 && buf[1] == 0x01);

    /* Check message length: 0 */
    assert(buf[2] == 0x00 && buf[3] == 0x00);

    /* Check magic cookie */
    assert(buf[4] == 0x21 && buf[5] == 0x12 &&
           buf[6] == 0xA4 && buf[7] == 0x42);

    /* Check transaction ID */
    assert(memcmp(buf + 8, txn_id, 12) == 0);

    printf("  PASS: build_request\n");
}

static void test_build_request_null_safety(void) {
    uint8_t txn_id[12] = {0};

    assert(md_stun_build_request(NULL, 20, txn_id) == -1);
    assert(md_stun_build_request((uint8_t[20]){0}, 20, NULL) == -1);

    /* Buffer too small */
    uint8_t small[10];
    assert(md_stun_build_request(small, sizeof(small), txn_id) == -1);

    printf("  PASS: build_request null safety\n");
}

static void test_parse_xor_mapped_ipv4(void) {
    uint8_t txn_id[12] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,
                           0x11,0x22,0x33,0x44,0x55,0x66};

    /* Build XOR-MAPPED-ADDRESS for 203.0.113.42:54321 */
    uint8_t attr_val[8];
    int attr_len = build_xor_mapped_ipv4(attr_val, sizeof(attr_val),
                                          txn_id, "203.0.113.42", 54321);
    assert(attr_len == 8);

    uint8_t resp[128];
    int resp_len = build_response(resp, sizeof(resp), txn_id,
                                  MD_STUN_ATTR_XOR_MAPPED_ADDR,
                                  attr_val, (uint16_t)attr_len);
    assert(resp_len > 0);

    MdStunResult result;
    int ret = md_stun_parse_response(resp, (size_t)resp_len, txn_id, &result);
    assert(ret == 0);
    assert(strcmp(result.ip, "203.0.113.42") == 0);
    assert(result.port == 54321);
    assert(result.is_ipv6 == false);

    printf("  PASS: parse XOR-MAPPED-ADDRESS IPv4\n");
}

static void test_parse_xor_mapped_ipv6(void) {
    uint8_t txn_id[12] = {0x01,0x02,0x03,0x04,0x05,0x06,
                           0x07,0x08,0x09,0x0A,0x0B,0x0C};

    /* Build XOR-MAPPED-ADDRESS for 2001:db8::1:8080 */
    uint8_t attr_val[20];
    int attr_len = build_xor_mapped_ipv6(attr_val, sizeof(attr_val),
                                          txn_id, "2001:db8::1", 8080);
    assert(attr_len == 20);

    uint8_t resp[128];
    int resp_len = build_response(resp, sizeof(resp), txn_id,
                                  MD_STUN_ATTR_XOR_MAPPED_ADDR,
                                  attr_val, (uint16_t)attr_len);
    assert(resp_len > 0);

    MdStunResult result;
    int ret = md_stun_parse_response(resp, (size_t)resp_len, txn_id, &result);
    assert(ret == 0);
    assert(result.port == 8080);
    assert(result.is_ipv6 == true);

    /* Verify IPv6 address (inet_ntop may compress — compare parsed form) */
    struct in6_addr parsed, expected;
    assert(inet_pton(AF_INET6, result.ip, &parsed) == 1);
    assert(inet_pton(AF_INET6, "2001:db8::1", &expected) == 1);
    assert(memcmp(&parsed, &expected, 16) == 0);

    printf("  PASS: parse XOR-MAPPED-ADDRESS IPv6\n");
}

static void test_parse_mapped_addr_fallback(void) {
    uint8_t txn_id[12] = {0};

    /* Build plain MAPPED-ADDRESS (no XOR) for 10.0.0.1:1234 */
    uint8_t attr_val[8];
    int attr_len = build_mapped_ipv4(attr_val, sizeof(attr_val),
                                      "10.0.0.1", 1234);
    assert(attr_len == 8);

    uint8_t resp[128];
    int resp_len = build_response(resp, sizeof(resp), txn_id,
                                  MD_STUN_ATTR_MAPPED_ADDR,
                                  attr_val, (uint16_t)attr_len);
    assert(resp_len > 0);

    MdStunResult result;
    int ret = md_stun_parse_response(resp, (size_t)resp_len, txn_id, &result);
    assert(ret == 0);
    assert(strcmp(result.ip, "10.0.0.1") == 0);
    assert(result.port == 1234);
    assert(result.is_ipv6 == false);

    printf("  PASS: parse MAPPED-ADDRESS fallback (no XOR)\n");
}

static void test_xor_preferred_over_mapped(void) {
    uint8_t txn_id[12] = {0x55,0x66,0x77,0x88,0x99,0xAA,
                           0xBB,0xCC,0xDD,0xEE,0xFF,0x00};

    /* Build a response with BOTH MAPPED-ADDRESS and XOR-MAPPED-ADDRESS.
     * The parser should prefer XOR-MAPPED-ADDRESS. */

    /* MAPPED-ADDRESS: 10.0.0.1:1111 */
    uint8_t mapped_val[8];
    build_mapped_ipv4(mapped_val, sizeof(mapped_val), "10.0.0.1", 1111);

    /* XOR-MAPPED-ADDRESS: 192.168.1.100:2222 */
    uint8_t xor_val[8];
    build_xor_mapped_ipv4(xor_val, sizeof(xor_val), txn_id,
                           "192.168.1.100", 2222);

    /* Manually build response with two attributes */
    uint8_t resp[256];
    memset(resp, 0, sizeof(resp));

    /* Attribute 1: MAPPED-ADDRESS */
    uint8_t *a1 = resp + MD_STUN_HEADER_SIZE;
    put_u16(a1 + 0, MD_STUN_ATTR_MAPPED_ADDR);
    put_u16(a1 + 2, 8);
    memcpy(a1 + 4, mapped_val, 8);

    /* Attribute 2: XOR-MAPPED-ADDRESS */
    uint8_t *a2 = a1 + 12; /* 4 header + 8 value */
    put_u16(a2 + 0, MD_STUN_ATTR_XOR_MAPPED_ADDR);
    put_u16(a2 + 2, 8);
    memcpy(a2 + 4, xor_val, 8);

    uint16_t msg_len = 24; /* 12 + 12 */

    /* Header */
    put_u16(resp + 0, MD_STUN_BINDING_RESPONSE);
    put_u16(resp + 2, msg_len);
    put_u32(resp + 4, MD_STUN_MAGIC_COOKIE);
    memcpy(resp + 8, txn_id, 12);

    size_t total = MD_STUN_HEADER_SIZE + msg_len;

    MdStunResult result;
    int ret = md_stun_parse_response(resp, total, txn_id, &result);
    assert(ret == 0);
    assert(strcmp(result.ip, "192.168.1.100") == 0);
    assert(result.port == 2222);

    printf("  PASS: XOR-MAPPED-ADDRESS preferred over MAPPED-ADDRESS\n");
}

static void test_parse_error_truncated(void) {
    uint8_t txn_id[12] = {0};
    MdStunResult result;

    /* Too short to be a STUN header */
    uint8_t short_buf[10] = {0};
    assert(md_stun_parse_response(short_buf, sizeof(short_buf),
                                  txn_id, &result) == -1);

    printf("  PASS: parse error — truncated message\n");
}

static void test_parse_error_wrong_type(void) {
    uint8_t txn_id[12] = {0};
    uint8_t resp[32];
    memset(resp, 0, sizeof(resp));

    /* Build a header with wrong message type (Binding Request instead of Response) */
    put_u16(resp + 0, MD_STUN_BINDING_REQUEST);
    put_u16(resp + 2, 0);
    put_u32(resp + 4, MD_STUN_MAGIC_COOKIE);
    memcpy(resp + 8, txn_id, 12);

    MdStunResult result;
    assert(md_stun_parse_response(resp, MD_STUN_HEADER_SIZE,
                                  txn_id, &result) == -1);

    printf("  PASS: parse error — wrong message type\n");
}

static void test_parse_error_bad_cookie(void) {
    uint8_t txn_id[12] = {0};
    uint8_t resp[32];
    memset(resp, 0, sizeof(resp));

    put_u16(resp + 0, MD_STUN_BINDING_RESPONSE);
    put_u16(resp + 2, 0);
    put_u32(resp + 4, 0xDEADBEEF); /* wrong magic cookie */
    memcpy(resp + 8, txn_id, 12);

    MdStunResult result;
    assert(md_stun_parse_response(resp, MD_STUN_HEADER_SIZE,
                                  txn_id, &result) == -1);

    printf("  PASS: parse error — bad magic cookie\n");
}

static void test_parse_error_txn_mismatch(void) {
    uint8_t txn_id[12]  = {1,2,3,4,5,6,7,8,9,10,11,12};
    uint8_t wrong_id[12] = {0};
    uint8_t resp[32];
    memset(resp, 0, sizeof(resp));

    put_u16(resp + 0, MD_STUN_BINDING_RESPONSE);
    put_u16(resp + 2, 0);
    put_u32(resp + 4, MD_STUN_MAGIC_COOKIE);
    memcpy(resp + 8, wrong_id, 12); /* different txn ID */

    MdStunResult result;
    assert(md_stun_parse_response(resp, MD_STUN_HEADER_SIZE,
                                  txn_id, &result) == -1);

    printf("  PASS: parse error — transaction ID mismatch\n");
}

static void test_parse_no_mapped_attr(void) {
    uint8_t txn_id[12] = {0};
    uint8_t resp[32];
    memset(resp, 0, sizeof(resp));

    /* Valid header but no attributes */
    put_u16(resp + 0, MD_STUN_BINDING_RESPONSE);
    put_u16(resp + 2, 0);
    put_u32(resp + 4, MD_STUN_MAGIC_COOKIE);
    memcpy(resp + 8, txn_id, 12);

    MdStunResult result;
    assert(md_stun_parse_response(resp, MD_STUN_HEADER_SIZE,
                                  txn_id, &result) == -1);

    printf("  PASS: parse error — no MAPPED-ADDRESS attribute\n");
}

static void test_parse_null_safety(void) {
    uint8_t txn_id[12] = {0};
    uint8_t buf[32] = {0};
    MdStunResult result;

    assert(md_stun_parse_response(NULL, 20, txn_id, &result) == -1);
    assert(md_stun_parse_response(buf, 20, NULL, &result) == -1);
    assert(md_stun_parse_response(buf, 20, txn_id, NULL) == -1);

    printf("  PASS: parse null safety\n");
}

static void test_various_ipv4_addresses(void) {
    uint8_t txn_id[12] = {0};

    /* Test with several representative IPv4 addresses */
    const struct {
        const char *ip;
        uint16_t port;
    } cases[] = {
        { "1.2.3.4",         1 },
        { "255.255.255.255", 65535 },
        { "0.0.0.0",         0 },
        { "192.168.0.1",     80 },
        { "172.16.254.1",    443 },
        { "8.8.8.8",         53 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t attr_val[8];
        int attr_len = build_xor_mapped_ipv4(attr_val, sizeof(attr_val),
                                              txn_id, cases[i].ip, cases[i].port);
        assert(attr_len == 8);

        uint8_t resp[128];
        int resp_len = build_response(resp, sizeof(resp), txn_id,
                                      MD_STUN_ATTR_XOR_MAPPED_ADDR,
                                      attr_val, (uint16_t)attr_len);
        assert(resp_len > 0);

        MdStunResult result;
        int ret = md_stun_parse_response(resp, (size_t)resp_len, txn_id, &result);
        assert(ret == 0);
        assert(strcmp(result.ip, cases[i].ip) == 0);
        assert(result.port == cases[i].port);
    }

    printf("  PASS: various IPv4 addresses (%zu cases)\n",
           sizeof(cases) / sizeof(cases[0]));
}

static void test_port_edge_cases(void) {
    uint8_t txn_id[12] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                           0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

    /* Port 0 and port 65535 with all-ones txn_id */
    uint16_t ports[] = { 0, 1, 1023, 1024, 32768, 65534, 65535 };
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        uint8_t attr_val[8];
        build_xor_mapped_ipv4(attr_val, sizeof(attr_val),
                               txn_id, "1.1.1.1", ports[i]);

        uint8_t resp[128];
        int resp_len = build_response(resp, sizeof(resp), txn_id,
                                      MD_STUN_ATTR_XOR_MAPPED_ADDR,
                                      attr_val, 8);

        MdStunResult result;
        int ret = md_stun_parse_response(resp, (size_t)resp_len, txn_id, &result);
        assert(ret == 0);
        assert(result.port == ports[i]);
    }

    printf("  PASS: port edge cases (%zu cases)\n",
           sizeof(ports) / sizeof(ports[0]));
}

static void test_discover_simple_null(void) {
    /* md_stun_discover with NULL buf should fail gracefully */
    assert(md_stun_discover("stun.l.google.com", NULL, 64) == -1);

    char buf[64];
    assert(md_stun_discover("stun.l.google.com", buf, 0) == -1);

    printf("  PASS: discover simple API null safety\n");
}

static void test_discover_full_null(void) {
    /* md_stun_discover_full with NULL result should fail */
    assert(md_stun_discover_full("stun.l.google.com", 0, NULL, 0) == -1);

    printf("  PASS: discover_full null result\n");
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    printf("test_stun:\n");

    test_build_request();
    test_build_request_null_safety();
    test_parse_xor_mapped_ipv4();
    test_parse_xor_mapped_ipv6();
    test_parse_mapped_addr_fallback();
    test_xor_preferred_over_mapped();
    test_parse_error_truncated();
    test_parse_error_wrong_type();
    test_parse_error_bad_cookie();
    test_parse_error_txn_mismatch();
    test_parse_no_mapped_attr();
    test_parse_null_safety();
    test_various_ipv4_addresses();
    test_port_edge_cases();
    test_discover_simple_null();
    test_discover_full_null();

    printf("test_stun: ALL %d TESTS PASSED\n", 16);
    return 0;
}
