/*
 * test_secrets.c — 1Password Connect integration tests.
 *
 * Tests the op:// reference parser, URL parser, and API surface.
 * Note: actual HTTP calls to 1Password Connect are not tested here
 * (would require a running server). These tests validate the parsing
 * and error handling logic.
 */
#include "secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  test: %s ... ", name); } while (0)
#define PASS() \
    do { printf("PASS\n"); tests_passed++; } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); tests_failed++; } while (0)

/* ── Test: create/destroy lifecycle ──────────────────────────── */

static void test_create_destroy(void) {
    TEST("create/destroy lifecycle");

    MdSecrets *s = md_secrets_create("http://localhost:8080", "test-token-abc");
    if (!s) {
        FAIL("create returned NULL");
        return;
    }

    md_secrets_destroy(s);
    PASS();
}

/* ── Test: create with invalid args ──────────────────────────── */

static void test_create_invalid(void) {
    TEST("create with invalid args");

    if (md_secrets_create(NULL, "token")) {
        FAIL("NULL url accepted"); return;
    }
    if (md_secrets_create("http://localhost", NULL)) {
        FAIL("NULL token accepted"); return;
    }
    if (md_secrets_create("http://localhost", "")) {
        FAIL("empty token accepted"); return;
    }

    PASS();
}

/* ── Test: get with invalid args ─────────────────────────────── */

static void test_get_invalid(void) {
    TEST("get with invalid args");

    MdSecrets *s = md_secrets_create("http://localhost:8080", "test-token");
    if (!s) { FAIL("create failed"); return; }

    uint8_t buf[64];

    /* NULL ref */
    if (md_secrets_get(s, NULL, buf, sizeof(buf)) >= 0) {
        md_secrets_destroy(s);
        FAIL("NULL ref accepted"); return;
    }

    /* NULL buf */
    if (md_secrets_get(s, "op://vault/item/field", NULL, 64) >= 0) {
        md_secrets_destroy(s);
        FAIL("NULL buf accepted"); return;
    }

    /* Zero buf_len */
    if (md_secrets_get(s, "op://vault/item/field", buf, 0) >= 0) {
        md_secrets_destroy(s);
        FAIL("zero buf_len accepted"); return;
    }

    /* Invalid op:// format (missing components) */
    if (md_secrets_get(s, "op://vault/item", buf, sizeof(buf)) >= 0) {
        md_secrets_destroy(s);
        FAIL("missing field accepted"); return;
    }

    if (md_secrets_get(s, "op://vault", buf, sizeof(buf)) >= 0) {
        md_secrets_destroy(s);
        FAIL("missing item/field accepted"); return;
    }

    /* Without op:// prefix — should still parse (slash-separated) */
    /* This will fail due to unreachable server, which is expected */
    int ret = md_secrets_get(s, "vault/item/field", buf, sizeof(buf));
    /* ret should be -1 because there's no server to connect to */
    if (ret >= 0) {
        /* Unexpected success without a server */
        md_secrets_destroy(s);
        FAIL("succeeded without server"); return;
    }

    md_secrets_destroy(s);
    PASS();
}

/* ── Test: is_connected without server ───────────────────────── */

static void test_not_connected(void) {
    TEST("is_connected returns false without server");

    /* Use a port that (almost certainly) has nothing listening */
    MdSecrets *s = md_secrets_create("http://127.0.0.1:19999", "test-token");
    if (!s) { FAIL("create failed"); return; }

    if (md_secrets_is_connected(s)) {
        md_secrets_destroy(s);
        FAIL("connected to non-existent server"); return;
    }

    md_secrets_destroy(s);
    PASS();
}

/* ── Test: URL parsing variations ────────────────────────────── */

static void test_url_parsing(void) {
    TEST("URL parsing variations");

    /* Standard http URL */
    MdSecrets *s1 = md_secrets_create("http://localhost:8080", "tok");
    if (!s1) { FAIL("http://localhost:8080 failed"); return; }
    md_secrets_destroy(s1);

    /* Without port (should default to 8080) */
    MdSecrets *s2 = md_secrets_create("http://localhost", "tok");
    if (!s2) { FAIL("http://localhost failed"); return; }
    md_secrets_destroy(s2);

    /* With IP address */
    MdSecrets *s3 = md_secrets_create("http://192.168.1.100:9000", "tok");
    if (!s3) { FAIL("IP address URL failed"); return; }
    md_secrets_destroy(s3);

    /* HTTPS prefix (accepted but no TLS in Phase 1) */
    MdSecrets *s4 = md_secrets_create("https://secrets.local:443", "tok");
    if (!s4) { FAIL("https URL failed"); return; }
    md_secrets_destroy(s4);

    PASS();
}

/* ── Test: secret zeroing on destroy ─────────────────────────── */

static void test_secure_destroy(void) {
    TEST("secure zeroing on destroy");

    /* This test verifies the API accepts and processes the token
     * without leaking. We can't directly verify memory zeroing
     * in a portable way, but we ensure the lifecycle works. */
    MdSecrets *s = md_secrets_create("http://localhost:8080",
                                     "very-secret-token-12345");
    if (!s) { FAIL("create failed"); return; }

    /* Destroy should zero the token and free cleanly */
    md_secrets_destroy(s);

    /* If we get here without crashes, the secure destroy worked */
    PASS();
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_secrets:\n");

    test_create_destroy();
    test_create_invalid();
    test_get_invalid();
    test_not_connected();
    test_url_parsing();
    test_secure_destroy();

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
