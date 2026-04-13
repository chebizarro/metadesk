/*
 * test_signer_nip5f.c — NIP-5F signer backend integration tests.
 *
 * Starts an in-process NIP-5F server on a temp Unix socket, then
 * exercises the full MdSigner NIP-5F vtable through a real socket
 * connection. Uses nostrc built-in handlers with NOSTR_SIGNER_SECKEY_HEX.
 *
 * Tests:
 *   1. Create + destroy lifecycle via real socket
 *   2. get_pubkey returns correct hex
 *   3. sign_event returns valid signed JSON with correct signature
 *   4. nip44 encrypt/decrypt roundtrip
 *   5. Type and state checks
 *   6. Nonexistent socket fails gracefully
 *   7. Multiple operations on same connection
 *   8. NOSTR_SIGNER_SOCK env var path resolution
 *
 * Requires: -DMD_SIGNER_ENABLE_NIP5F + libnostr-nip5f linked
 */
#include "signer.h"

#include <nostr/nip5f/nip5f.h>
#include <nostr-keys.h>
#include <nostr-event.h>
#include <json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  test: %s ... ", name); } while (0)
#define PASS() \
    do { printf("PASS\n"); tests_passed++; } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); tests_failed++; } while (0)

/* ── Test key ────────────────────────────────────────────────── */
static const char *TEST_SK =
    "a665a45920422f9d417e4867efdc4fb8a04a1f3fff1fa07e998e86f7f7a27ae3";

/* ── Server fixture ──────────────────────────────────────────── */

static char *g_sock_path = NULL;
static void *g_server = NULL;
static char *g_expected_pk = NULL;

static int server_setup(void) {
    /* Generate unique socket path */
    char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/nostr-nip5f-md-test-%ld-%d.sock",
             (long)time(NULL), (int)getpid());
    g_sock_path = strdup(buf);

    /* Set key for built-in handlers */
    setenv("NOSTR_SIGNER_SECKEY_HEX", TEST_SK, 1);
    setenv("NOSTR_TEST_MODE", "1", 1);

    g_expected_pk = nostr_key_get_public(TEST_SK);
    if (!g_expected_pk) return -1;

    /* Start server */
    if (nostr_nip5f_server_start(g_sock_path, &g_server) != 0)
        return -1;

    /* Use built-in handlers (NULL = defaults) */
    nostr_nip5f_server_set_handlers(g_server,
        NULL, NULL, NULL, NULL, NULL, NULL);

    return 0;
}

static void server_teardown(void) {
    if (g_server) {
        nostr_nip5f_server_stop(g_server);
        g_server = NULL;
    }
    if (g_sock_path) {
        unlink(g_sock_path);
        free(g_sock_path);
        g_sock_path = NULL;
    }
    free(g_expected_pk);
    g_expected_pk = NULL;
    unsetenv("NOSTR_SIGNER_SECKEY_HEX");
    unsetenv("NOSTR_TEST_MODE");
}

/* ── Test: nonexistent socket fails ──────────────────────────── */

static void test_nonexistent_socket(void) {
    TEST("NIP-5F nonexistent socket fails");

    MdSigner *s = md_signer_create_nip5f("/nonexistent/socket.sock");
    if (s) {
        md_signer_destroy(s);
        FAIL("connected to nonexistent socket");
        return;
    }

    PASS();
}

/* ── Test: type name ─────────────────────────────────────────── */

static void test_type_name(void) {
    TEST("NIP-5F type name");

    const char *name = md_signer_type_name(MD_SIGNER_NIP5F);
    if (!name || strcmp(name, "NIP-5F (Unix socket)") != 0) {
        FAIL("wrong type name");
        return;
    }

    PASS();
}

/* ── Test: lifecycle via real socket ─────────────────────────── */

static void test_lifecycle(void) {
    TEST("NIP-5F create/destroy via real socket");

    MdSigner *s = md_signer_create_nip5f(g_sock_path);
    if (!s) { FAIL("create failed"); return; }

    if (md_signer_get_type(s) != MD_SIGNER_NIP5F) {
        md_signer_destroy(s);
        FAIL("wrong type");
        return;
    }

    if (!md_signer_is_ready(s)) {
        md_signer_destroy(s);
        FAIL("not ready");
        return;
    }

    md_signer_destroy(s);
    PASS();
}

/* ── Test: get_pubkey correct hex ────────────────────────────── */

static void test_get_pubkey(void) {
    TEST("NIP-5F get_pubkey returns correct hex");

    MdSigner *s = md_signer_create_nip5f(g_sock_path);
    if (!s) { FAIL("create failed"); return; }

    char *pk = NULL;
    int ret = md_signer_get_pubkey(s, &pk);
    if (ret != MD_SIGNER_OK || !pk) {
        md_signer_destroy(s);
        FAIL("get_pubkey failed");
        return;
    }

    if (strlen(pk) != 64) {
        printf("len=%zu ", strlen(pk));
        free(pk);
        md_signer_destroy(s);
        FAIL("not 64 chars");
        return;
    }

    if (strcmp(pk, g_expected_pk) != 0) {
        printf("got=%.*s exp=%.*s ", 16, pk, 16, g_expected_pk);
        free(pk);
        md_signer_destroy(s);
        FAIL("pubkey mismatch");
        return;
    }

    free(pk);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: pubkey caching ────────────────────────────────────── */

static void test_pubkey_caching(void) {
    TEST("NIP-5F pubkey caching");

    MdSigner *s = md_signer_create_nip5f(g_sock_path);
    if (!s) { FAIL("create failed"); return; }

    char *pk1 = NULL, *pk2 = NULL;
    md_signer_get_pubkey(s, &pk1);
    md_signer_get_pubkey(s, &pk2);

    if (!pk1 || !pk2 || strcmp(pk1, pk2) != 0) {
        free(pk1); free(pk2);
        md_signer_destroy(s);
        FAIL("cached pubkeys differ");
        return;
    }

    free(pk1);
    free(pk2);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: sign_event ────────────────────────────────────────── */

static void test_sign_event(void) {
    TEST("NIP-5F sign_event with signature verification");

    MdSigner *s = md_signer_create_nip5f(g_sock_path);
    if (!s) { FAIL("create failed"); return; }

    /* Build unsigned event */
    char event_json[512];
    snprintf(event_json, sizeof(event_json),
             "{\"kind\":1,\"content\":\"nip5f test\",\"tags\":[],"
             "\"created_at\":1704067200,\"pubkey\":\"%s\"}", g_expected_pk);

    char *signed_json = NULL;
    int ret = md_signer_sign_event(s, event_json, &signed_json);
    if (ret != MD_SIGNER_OK || !signed_json) {
        md_signer_destroy(s);
        FAIL("sign_event failed");
        return;
    }

    /* Verify contains sig and id */
    if (!strstr(signed_json, "\"sig\"") || !strstr(signed_json, "\"id\"")) {
        free(signed_json);
        md_signer_destroy(s);
        FAIL("missing sig/id fields");
        return;
    }

    /* Deserialize and verify signature */
    NostrEvent *ev = nostr_event_new();
    if (!ev || nostr_event_deserialize(ev, signed_json) != 0) {
        if (ev) nostr_event_free(ev);
        free(signed_json);
        md_signer_destroy(s);
        FAIL("deserialize failed");
        return;
    }
    free(signed_json);

    /* Check pubkey */
    const char *evpk = nostr_event_get_pubkey(ev);
    if (!evpk || strcmp(evpk, g_expected_pk) != 0) {
        nostr_event_free(ev);
        md_signer_destroy(s);
        FAIL("pubkey mismatch in signed event");
        return;
    }

    /* Verify cryptographic signature */
    if (!nostr_event_check_signature(ev)) {
        nostr_event_free(ev);
        md_signer_destroy(s);
        FAIL("signature verification failed");
        return;
    }

    nostr_event_free(ev);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: NIP-44 encrypt/decrypt roundtrip ──────────────────── */

static void test_nip44_roundtrip(void) {
    TEST("NIP-5F NIP-44 encrypt/decrypt roundtrip");

    MdSigner *s = md_signer_create_nip5f(g_sock_path);
    if (!s) { FAIL("create failed"); return; }

    /* Encrypt to self (simplest valid case) */
    const char *plaintext = "hello from nip5f signer test";
    char *ciphertext = NULL;
    int ret = md_signer_nip44_encrypt(s, g_expected_pk, plaintext, &ciphertext);
    if (ret != MD_SIGNER_OK || !ciphertext) {
        md_signer_destroy(s);
        FAIL("encrypt failed");
        return;
    }

    /* Ciphertext must differ from plaintext */
    if (strcmp(ciphertext, plaintext) == 0) {
        free(ciphertext);
        md_signer_destroy(s);
        FAIL("ciphertext == plaintext");
        return;
    }

    /* Decrypt */
    char *decrypted = NULL;
    ret = md_signer_nip44_decrypt(s, g_expected_pk, ciphertext, &decrypted);
    free(ciphertext);
    if (ret != MD_SIGNER_OK || !decrypted) {
        md_signer_destroy(s);
        FAIL("decrypt failed");
        return;
    }

    if (strcmp(decrypted, plaintext) != 0) {
        printf("expected='%s' got='%s' ", plaintext, decrypted);
        free(decrypted);
        md_signer_destroy(s);
        FAIL("plaintext mismatch");
        return;
    }

    free(decrypted);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: multiple operations on same connection ────────────── */

static void test_multiple_operations(void) {
    TEST("NIP-5F multiple operations on same connection");

    MdSigner *s = md_signer_create_nip5f(g_sock_path);
    if (!s) { FAIL("create failed"); return; }

    int ok = 1;
    for (int i = 0; i < 5 && ok; i++) {
        char event_json[512];
        snprintf(event_json, sizeof(event_json),
                 "{\"kind\":1,\"content\":\"msg %d\",\"tags\":[],"
                 "\"created_at\":%ld,\"pubkey\":\"%s\"}",
                 i, 1704067200L + i, g_expected_pk);

        char *signed_json = NULL;
        int ret = md_signer_sign_event(s, event_json, &signed_json);
        if (ret != MD_SIGNER_OK || !signed_json) {
            ok = 0; break;
        }

        /* Quick sanity: has sig */
        if (!strstr(signed_json, "\"sig\"")) {
            free(signed_json); ok = 0; break;
        }
        free(signed_json);
    }

    md_signer_destroy(s);

    if (!ok) { FAIL("operation failed during loop"); return; }
    PASS();
}

/* ── Test: NOSTR_SIGNER_SOCK env var ─────────────────────────── */

static void test_env_var_socket_path(void) {
    TEST("NIP-5F NOSTR_SIGNER_SOCK env var");

    /* Set env to our test socket */
    setenv("NOSTR_SIGNER_SOCK", g_sock_path, 1);

    /* Create with NULL path (should use env var) */
    MdSigner *s = md_signer_create_nip5f(NULL);
    unsetenv("NOSTR_SIGNER_SOCK");

    if (!s) { FAIL("create with env var failed"); return; }

    char *pk = NULL;
    md_signer_get_pubkey(s, &pk);
    if (!pk || strcmp(pk, g_expected_pk) != 0) {
        free(pk);
        md_signer_destroy(s);
        FAIL("pubkey mismatch via env var");
        return;
    }

    free(pk);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: destroy NULL safe ─────────────────────────────────── */

static void test_destroy_null(void) {
    TEST("NIP-5F destroy NULL safe");
    md_signer_destroy(NULL);
    PASS();
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_signer_nip5f:\n");

    /* Tests that don't need server */
    test_nonexistent_socket();
    test_type_name();
    test_destroy_null();

    /* Start server for remaining tests */
    if (server_setup() != 0) {
        fprintf(stderr, "  FATAL: server setup failed, skipping socket tests\n");
        server_teardown();
        printf("\nResults: %d passed, %d failed (server tests skipped)\n",
               tests_passed, tests_failed);
        return tests_failed > 0 ? 1 : 0;
    }

    test_lifecycle();
    test_get_pubkey();
    test_pubkey_caching();
    test_sign_event();
    test_nip44_roundtrip();
    test_multiple_operations();
    test_env_var_socket_path();

    server_teardown();

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
