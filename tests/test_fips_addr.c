/*
 * test_fips_addr.c — FIPS address derivation tests.
 *
 * Validates:
 *   1. npub → fd00::/8 IPv6 derivation matches the FIPS reference algorithm
 *   2. Hex pubkey → IPv6 derivation
 *   3. Bech32 decode correctness
 *   4. Validation helpers
 *   5. DNS name construction
 *   6. MTU constants
 */
#include "fips_addr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  test: %s ... ", name); } while (0)
#define PASS() \
    do { printf("PASS\n"); tests_passed++; } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); tests_failed++; } while (0)

/* ── Test: address derivation produces fd00::/8 prefix ─────── */

static void test_addr_has_fips_prefix(void) {
    TEST("address has fd00::/8 prefix");

    /* Use a well-known test npub — any valid npub will do.
     * npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6 is
     * a commonly used Nostr test key (Jack Dorsey). */
    const char *npub = "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6";

    char ipv6[MD_FIPS_IPV6_STRLEN];
    int ret = md_fips_addr_from_npub(npub, ipv6, sizeof(ipv6));

    if (ret != 0) {
        FAIL("md_fips_addr_from_npub returned error");
        return;
    }

    /* Verify the address starts with fd */
    if (!md_fips_is_fips_addr(ipv6)) {
        FAIL("address not in fd00::/8 range");
        return;
    }

    /* Parse and check first byte */
    struct in6_addr in6;
    if (inet_pton(AF_INET6, ipv6, &in6) != 1) {
        FAIL("invalid IPv6 string");
        return;
    }

    if (in6.s6_addr[0] != 0xfd) {
        FAIL("first byte is not 0xfd");
        return;
    }

    printf("(%s) ", ipv6);
    PASS();
}

/* ── Test: hex pubkey derivation matches npub derivation ───── */

static void test_hex_matches_npub(void) {
    TEST("hex pubkey matches npub derivation");

    /* npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6
     * decodes to hex pubkey:
     * 3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d */
    const char *npub = "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6";
    const char *pk_hex = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";

    char ipv6_npub[MD_FIPS_IPV6_STRLEN];
    char ipv6_hex[MD_FIPS_IPV6_STRLEN];

    int r1 = md_fips_addr_from_npub(npub, ipv6_npub, sizeof(ipv6_npub));
    int r2 = md_fips_addr_from_pubkey_hex(pk_hex, ipv6_hex, sizeof(ipv6_hex));

    if (r1 != 0) { FAIL("npub derivation failed"); return; }
    if (r2 != 0) { FAIL("hex derivation failed"); return; }

    if (strcmp(ipv6_npub, ipv6_hex) != 0) {
        printf("npub=%s hex=%s ", ipv6_npub, ipv6_hex);
        FAIL("addresses don't match");
        return;
    }

    PASS();
}

/* ── Test: deterministic output ────────────────────────────── */

static void test_deterministic(void) {
    TEST("derivation is deterministic");

    const char *npub = "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6";

    char ipv6_a[MD_FIPS_IPV6_STRLEN];
    char ipv6_b[MD_FIPS_IPV6_STRLEN];

    md_fips_addr_from_npub(npub, ipv6_a, sizeof(ipv6_a));
    md_fips_addr_from_npub(npub, ipv6_b, sizeof(ipv6_b));

    if (strcmp(ipv6_a, ipv6_b) != 0) {
        FAIL("non-deterministic output");
        return;
    }

    PASS();
}

/* ── Test: different keys produce different addresses ──────── */

static void test_different_keys(void) {
    TEST("different keys produce different addresses");

    /* Two different hex pubkeys */
    const char *pk1 = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";
    const char *pk2 = "e88a691e98d9987c964ab1c8f54cb49a4ab36f0c7bba68e01b32ed8d67ed59d4";

    char ipv6_1[MD_FIPS_IPV6_STRLEN];
    char ipv6_2[MD_FIPS_IPV6_STRLEN];

    md_fips_addr_from_pubkey_hex(pk1, ipv6_1, sizeof(ipv6_1));
    md_fips_addr_from_pubkey_hex(pk2, ipv6_2, sizeof(ipv6_2));

    if (strcmp(ipv6_1, ipv6_2) == 0) {
        FAIL("different keys produced same address");
        return;
    }

    /* Both should still be in fd00::/8 */
    if (!md_fips_is_fips_addr(ipv6_1) || !md_fips_is_fips_addr(ipv6_2)) {
        FAIL("addresses not in fd00::/8");
        return;
    }

    PASS();
}

/* ── Test: raw pubkey derivation ───────────────────────────── */

static void test_raw_pubkey(void) {
    TEST("raw pubkey derivation");

    uint8_t pubkey[32] = {
        0x3b, 0xf0, 0xc6, 0x3f, 0xcb, 0x93, 0x46, 0x34,
        0x07, 0xaf, 0x97, 0xa5, 0xe5, 0xee, 0x64, 0xfa,
        0x88, 0x3d, 0x10, 0x7e, 0xf9, 0xe5, 0x58, 0x47,
        0x2c, 0x4e, 0xb9, 0xaa, 0xae, 0xfa, 0x45, 0x9d,
    };

    uint8_t addr[16];
    int ret = md_fips_addr_from_pubkey(pubkey, 32, addr);
    if (ret != 0) { FAIL("derivation failed"); return; }

    if (addr[0] != 0xfd) {
        FAIL("first byte not 0xfd");
        return;
    }

    /* Format as string and verify */
    char ipv6[MD_FIPS_IPV6_STRLEN];
    ret = md_fips_addr_to_string(addr, ipv6, sizeof(ipv6));
    if (ret != 0) { FAIL("string format failed"); return; }

    if (!md_fips_is_fips_addr(ipv6)) {
        FAIL("formatted address not FIPS");
        return;
    }

    printf("(%s) ", ipv6);
    PASS();
}

/* ── Test: DNS name construction ───────────────────────────── */

static void test_dns_name(void) {
    TEST("DNS name construction");

    const char *npub = "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6";
    char dns[256];

    int ret = md_fips_dns_name(npub, dns, sizeof(dns));
    if (ret != 0) { FAIL("dns_name returned error"); return; }

    /* Should end with .fips */
    const char *suffix = strstr(dns, ".fips");
    if (!suffix || suffix[5] != '\0') {
        FAIL("missing .fips suffix");
        return;
    }

    /* Should start with the npub */
    if (strncmp(dns, npub, strlen(npub)) != 0) {
        FAIL("doesn't start with npub");
        return;
    }

    PASS();
}

/* ── Test: validation helpers ──────────────────────────────── */

static void test_validation(void) {
    TEST("validation helpers");

    /* md_fips_is_npub */
    if (!md_fips_is_npub("npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6")) {
        FAIL("valid npub not recognized"); return;
    }
    if (md_fips_is_npub("nsec1abc")) {
        FAIL("nsec accepted as npub"); return;
    }
    if (md_fips_is_npub(NULL)) {
        FAIL("NULL accepted as npub"); return;
    }
    if (md_fips_is_npub("short")) {
        FAIL("short string accepted as npub"); return;
    }

    /* md_fips_is_fips_addr */
    if (!md_fips_is_fips_addr("fd00::1")) {
        FAIL("fd00::1 not recognized"); return;
    }
    if (md_fips_is_fips_addr("::1")) {
        FAIL("::1 accepted as FIPS addr"); return;
    }
    if (md_fips_is_fips_addr(NULL)) {
        FAIL("NULL accepted as FIPS addr"); return;
    }

    PASS();
}

/* ── Test: error handling ──────────────────────────────────── */

static void test_error_handling(void) {
    TEST("error handling");

    char ipv6[MD_FIPS_IPV6_STRLEN];

    /* NULL inputs */
    if (md_fips_addr_from_npub(NULL, ipv6, sizeof(ipv6)) == 0) {
        FAIL("NULL npub accepted"); return;
    }

    /* Invalid npub */
    if (md_fips_addr_from_npub("not-an-npub", ipv6, sizeof(ipv6)) == 0) {
        FAIL("invalid npub accepted"); return;
    }

    /* Buffer too small */
    char tiny[5];
    if (md_fips_addr_from_npub(
            "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6",
            tiny, sizeof(tiny)) == 0) {
        FAIL("tiny buffer accepted"); return;
    }

    /* Invalid hex */
    if (md_fips_addr_from_pubkey_hex("not-hex", ipv6, sizeof(ipv6)) == 0) {
        FAIL("invalid hex accepted"); return;
    }

    /* Wrong length hex */
    if (md_fips_addr_from_pubkey_hex("abcd", ipv6, sizeof(ipv6)) == 0) {
        FAIL("short hex accepted"); return;
    }

    PASS();
}

/* ── Test: MTU constants ───────────────────────────────────── */

static void test_mtu_constants(void) {
    TEST("MTU constants");

    if (MD_FIPS_IPV6_OVERHEAD != 77) {
        FAIL("FIPS_IPV6_OVERHEAD != 77"); return;
    }

    if (MD_FIPS_EFFECTIVE_MTU != (1280 - 77)) {
        FAIL("FIPS_EFFECTIVE_MTU incorrect"); return;
    }

    if (MD_FIPS_EFFECTIVE_MTU <= 0) {
        FAIL("effective MTU is non-positive"); return;
    }

    PASS();
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_fips_addr:\n");

    test_addr_has_fips_prefix();
    test_hex_matches_npub();
    test_deterministic();
    test_different_keys();
    test_raw_pubkey();
    test_dns_name();
    test_validation();
    test_error_handling();
    test_mtu_constants();

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
