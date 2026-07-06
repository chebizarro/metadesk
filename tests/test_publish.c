/*
 * metadesk — test_publish.c
 * NAT endpoint publication tests.
 *
 * Tests JSON serialization/deserialization of NAT endpoint data
 * and null safety. Network-dependent Nostr publication is tested
 * via integration tests.
 */
#include "../src/fips-nat/publish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Serialization tests ─────────────────────────────────────── */

static void test_endpoint_to_json_ipv4(void) {
    MdNatEndpoint ep = {
        .stun = {
            .port = 54321,
            .is_ipv6 = false,
        },
        .fips_port = 2121,
        .punch_port = 19800,
    };
    strncpy(ep.stun.ip, "203.0.113.42", sizeof(ep.stun.ip));

    char *json = md_nat_endpoint_to_json(&ep);
    assert(json != NULL);

    /* Verify key fields are present */
    assert(strstr(json, "\"v\":1") != NULL);
    assert(strstr(json, "\"ip\":\"203.0.113.42\"") != NULL);
    assert(strstr(json, "\"port\":54321") != NULL);
    assert(strstr(json, "\"proto\":\"udp\"") != NULL);
    assert(strstr(json, "\"fips_port\":2121") != NULL);
    assert(strstr(json, "\"punch_port\":19800") != NULL);

    /* IPv6 flag should NOT be present for IPv4 */
    assert(strstr(json, "\"ipv6\"") == NULL);

    free(json);
    printf("  PASS: endpoint to JSON (IPv4)\n");
}

static void test_endpoint_to_json_ipv6(void) {
    MdNatEndpoint ep = {
        .stun = {
            .port = 8080,
            .is_ipv6 = true,
        },
        .fips_port = 2121,
        .punch_port = 0,
    };
    strncpy(ep.stun.ip, "2001:db8::1", sizeof(ep.stun.ip));

    char *json = md_nat_endpoint_to_json(&ep);
    assert(json != NULL);

    assert(strstr(json, "\"ip\":\"2001:db8::1\"") != NULL);
    assert(strstr(json, "\"port\":8080") != NULL);
    assert(strstr(json, "\"ipv6\":true") != NULL);

    /* punch_port=0 should be omitted */
    assert(strstr(json, "\"punch_port\"") == NULL);

    free(json);
    printf("  PASS: endpoint to JSON (IPv6)\n");
}

static void test_endpoint_to_json_minimal(void) {
    /* Only required fields — fips_port and punch_port both 0 */
    MdNatEndpoint ep = {
        .stun = {
            .port = 1234,
            .is_ipv6 = false,
        },
        .fips_port = 0,
        .punch_port = 0,
    };
    strncpy(ep.stun.ip, "10.0.0.1", sizeof(ep.stun.ip));

    char *json = md_nat_endpoint_to_json(&ep);
    assert(json != NULL);

    assert(strstr(json, "\"ip\":\"10.0.0.1\"") != NULL);
    assert(strstr(json, "\"port\":1234") != NULL);
    assert(strstr(json, "\"fips_port\"") == NULL);
    assert(strstr(json, "\"punch_port\"") == NULL);

    free(json);
    printf("  PASS: endpoint to JSON (minimal)\n");
}

static void test_endpoint_to_json_null(void) {
    assert(md_nat_endpoint_to_json(NULL) == NULL);

    /* Empty IP should fail */
    MdNatEndpoint ep;
    memset(&ep, 0, sizeof(ep));
    assert(md_nat_endpoint_to_json(&ep) == NULL);

    printf("  PASS: endpoint to JSON null safety\n");
}

/* ── Deserialization tests ───────────────────────────────────── */

static void test_endpoint_from_json_full(void) {
    const char *json =
        "{\"v\":1,\"ip\":\"192.168.1.100\",\"port\":12345,"
        "\"proto\":\"udp\",\"fips_port\":2121,\"punch_port\":19800}";

    MdNatEndpoint ep;
    int ret = md_nat_endpoint_from_json(json, &ep);
    assert(ret == 0);
    assert(strcmp(ep.stun.ip, "192.168.1.100") == 0);
    assert(ep.stun.port == 12345);
    assert(ep.stun.is_ipv6 == false);
    assert(ep.fips_port == 2121);
    assert(ep.punch_port == 19800);

    printf("  PASS: endpoint from JSON (full)\n");
}

static void test_endpoint_from_json_ipv6(void) {
    const char *json =
        "{\"v\":1,\"ip\":\"2001:db8::1\",\"port\":443,\"proto\":\"udp\","
        "\"ipv6\":true,\"fips_port\":2121}";

    MdNatEndpoint ep;
    int ret = md_nat_endpoint_from_json(json, &ep);
    assert(ret == 0);
    assert(strcmp(ep.stun.ip, "2001:db8::1") == 0);
    assert(ep.stun.port == 443);
    assert(ep.stun.is_ipv6 == true);
    assert(ep.fips_port == 2121);
    assert(ep.punch_port == 0); /* not present → 0 */

    printf("  PASS: endpoint from JSON (IPv6)\n");
}

static void test_endpoint_from_json_minimal(void) {
    const char *json = "{\"v\":1,\"ip\":\"1.2.3.4\",\"port\":80}";

    MdNatEndpoint ep;
    int ret = md_nat_endpoint_from_json(json, &ep);
    assert(ret == 0);
    assert(strcmp(ep.stun.ip, "1.2.3.4") == 0);
    assert(ep.stun.port == 80);
    assert(ep.fips_port == 0);
    assert(ep.punch_port == 0);

    printf("  PASS: endpoint from JSON (minimal)\n");
}

static void test_endpoint_from_json_errors(void) {
    MdNatEndpoint ep;

    /* NULL inputs */
    assert(md_nat_endpoint_from_json(NULL, &ep) == -1);
    assert(md_nat_endpoint_from_json("{}", NULL) == -1);

    /* Missing version */
    assert(md_nat_endpoint_from_json("{\"ip\":\"1.2.3.4\",\"port\":80}", &ep) == -1);

    /* Missing IP */
    assert(md_nat_endpoint_from_json("{\"v\":1,\"port\":80}", &ep) == -1);

    /* Missing port */
    assert(md_nat_endpoint_from_json("{\"v\":1,\"ip\":\"1.2.3.4\"}", &ep) == -1);

    /* Invalid JSON */
    assert(md_nat_endpoint_from_json("not json", &ep) == -1);

    /* Empty IP */
    assert(md_nat_endpoint_from_json("{\"v\":1,\"ip\":\"\",\"port\":80}", &ep) == -1);

    /* Port = 0 */
    assert(md_nat_endpoint_from_json("{\"v\":1,\"ip\":\"1.2.3.4\",\"port\":0}", &ep) == -1);

    printf("  PASS: endpoint from JSON error cases\n");
}

/* ── Roundtrip tests ─────────────────────────────────────────── */

static void test_roundtrip_ipv4(void) {
    MdNatEndpoint orig = {
        .stun = {
            .port = 32768,
            .is_ipv6 = false,
        },
        .fips_port = 2121,
        .punch_port = 19800,
    };
    strncpy(orig.stun.ip, "172.16.254.1", sizeof(orig.stun.ip));

    char *json = md_nat_endpoint_to_json(&orig);
    assert(json != NULL);

    MdNatEndpoint parsed;
    int ret = md_nat_endpoint_from_json(json, &parsed);
    assert(ret == 0);
    assert(strcmp(parsed.stun.ip, orig.stun.ip) == 0);
    assert(parsed.stun.port == orig.stun.port);
    assert(parsed.stun.is_ipv6 == orig.stun.is_ipv6);
    assert(parsed.fips_port == orig.fips_port);
    assert(parsed.punch_port == orig.punch_port);

    free(json);
    printf("  PASS: roundtrip IPv4\n");
}

static void test_roundtrip_ipv6(void) {
    MdNatEndpoint orig = {
        .stun = {
            .port = 65535,
            .is_ipv6 = true,
        },
        .fips_port = 2121,
        .punch_port = 19801,
    };
    strncpy(orig.stun.ip, "fe80::1%en0", sizeof(orig.stun.ip));

    char *json = md_nat_endpoint_to_json(&orig);
    assert(json != NULL);

    MdNatEndpoint parsed;
    int ret = md_nat_endpoint_from_json(json, &parsed);
    assert(ret == 0);
    assert(strcmp(parsed.stun.ip, orig.stun.ip) == 0);
    assert(parsed.stun.port == orig.stun.port);
    assert(parsed.stun.is_ipv6 == true);
    assert(parsed.fips_port == orig.fips_port);
    assert(parsed.punch_port == orig.punch_port);

    free(json);
    printf("  PASS: roundtrip IPv6\n");
}

static void test_roundtrip_edge_ports(void) {
    uint16_t ports[] = { 1, 80, 443, 1024, 8080, 32768, 65534, 65535 };

    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        MdNatEndpoint orig = {
            .stun = { .port = ports[i] },
        };
        strncpy(orig.stun.ip, "10.0.0.1", sizeof(orig.stun.ip));

        char *json = md_nat_endpoint_to_json(&orig);
        assert(json != NULL);

        MdNatEndpoint parsed;
        int ret = md_nat_endpoint_from_json(json, &parsed);
        assert(ret == 0);
        assert(parsed.stun.port == ports[i]);

        free(json);
    }

    printf("  PASS: roundtrip edge ports (%zu cases)\n",
           sizeof(ports) / sizeof(ports[0]));
}

/* ── Publish/subscribe null safety ───────────────────────────── */

static void test_publish_null_safety(void) {
    MdNatEndpoint ep = { .stun = { .port = 1234 } };
    strncpy(ep.stun.ip, "1.2.3.4", sizeof(ep.stun.ip));

    assert(md_publish_nat_endpoint(NULL, &ep) == -1);
    assert(md_publish_nat_endpoint(NULL, NULL) == -1);
    /* Can't test with non-NULL nostr without a real connection */

    printf("  PASS: publish null safety\n");
}

static void test_subscribe_null_safety(void) {
    assert(md_subscribe_nat_endpoint(NULL, "abc") == -1);
    assert(md_subscribe_nat_endpoint(NULL, NULL) == -1);

    printf("  PASS: subscribe null safety\n");
}


/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    printf("test_publish:\n");

    /* Serialization */
    test_endpoint_to_json_ipv4();
    test_endpoint_to_json_ipv6();
    test_endpoint_to_json_minimal();
    test_endpoint_to_json_null();

    /* Deserialization */
    test_endpoint_from_json_full();
    test_endpoint_from_json_ipv6();
    test_endpoint_from_json_minimal();
    test_endpoint_from_json_errors();

    /* Roundtrips */
    test_roundtrip_ipv4();
    test_roundtrip_ipv6();
    test_roundtrip_edge_ports();

    /* Null safety */
    test_publish_null_safety();
    test_subscribe_null_safety();

    printf("test_publish: ALL %d TESTS PASSED\n", 14);
    return 0;
}
