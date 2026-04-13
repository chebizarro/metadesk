/*
 * test_signer_nip55l.c — NIP-55L signer backend integration tests.
 *
 * Tests the MdSigner NIP-55L backend by setting NOSTR_SIGNER_SECKEY_HEX
 * env var — the nostrc signer_ops resolve the key from the environment
 * without needing a D-Bus daemon. This exercises the full vtable path.
 *
 * Tests:
 *   1. Create + destroy lifecycle
 *   2. get_pubkey returns hex (not npub)
 *   3. sign_event returns full signed JSON
 *   4. nip44 encrypt/decrypt roundtrip
 *   5. Type and state checks
 *   6. Error handling (no key available)
 *   7. Stateless nature (multiple creates share same key)
 *
 * Requires: -DMD_SIGNER_ENABLE_NIP55L + libnostr-nip55l linked
 */
#include "signer.h"

#include <nostr-keys.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  test: %s ... ", name); } while (0)
#define PASS() \
    do { printf("PASS\n"); tests_passed++; } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); tests_failed++; } while (0)

/* ── Test key (deterministic) ────────────────────────────────── */
static const char *TEST_SK =
    "a665a45920422f9d417e4867efdc4fb8a04a1f3fff1fa07e998e86f7f7a27ae3";

/* Helper: set the env key for NIP-55L resolution */
static void set_test_key(void) {
    setenv("NOSTR_SIGNER_SECKEY_HEX", TEST_SK, 1);
}

static void clear_test_key(void) {
    unsetenv("NOSTR_SIGNER_SECKEY_HEX");
}

/* ── Test: no key → create fails gracefully ──────────────────── */

static void test_no_key_fails(void) {
    TEST("NIP-55L create fails without key");

    clear_test_key();

    MdSigner *s = md_signer_create_nip55l();
    if (s) {
        md_signer_destroy(s);
        FAIL("create succeeded without key");
        return;
    }

    PASS();
}

/* ── Test: lifecycle with env key ────────────────────────────── */

static void test_lifecycle(void) {
    TEST("NIP-55L create/destroy lifecycle");

    set_test_key();

    MdSigner *s = md_signer_create_nip55l();
    if (!s) { FAIL("create failed"); return; }

    if (md_signer_get_type(s) != MD_SIGNER_NIP55L) {
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

/* ── Test: type name ─────────────────────────────────────────── */

static void test_type_name(void) {
    TEST("NIP-55L type name");

    const char *name = md_signer_type_name(MD_SIGNER_NIP55L);
    if (!name || strcmp(name, "NIP-55L (D-Bus)") != 0) {
        FAIL("wrong type name");
        return;
    }

    PASS();
}

/* ── Test: get_pubkey returns hex ────────────────────────────── */

static void test_get_pubkey_hex(void) {
    TEST("NIP-55L get_pubkey returns hex");

    set_test_key();

    MdSigner *s = md_signer_create_nip55l();
    if (!s) { FAIL("create failed"); return; }

    char *pk = NULL;
    int ret = md_signer_get_pubkey(s, &pk);
    if (ret != MD_SIGNER_OK || !pk) {
        md_signer_destroy(s);
        FAIL("get_pubkey failed");
        return;
    }

    /* Must be 64 hex chars, not bech32 npub */
    if (strlen(pk) != 64) {
        printf("len=%zu ", strlen(pk));
        free(pk);
        md_signer_destroy(s);
        FAIL("not 64 chars");
        return;
    }

    /* Must not start with 'npub1' */
    if (strncmp(pk, "npub1", 5) == 0) {
        free(pk);
        md_signer_destroy(s);
        FAIL("got npub instead of hex");
        return;
    }

    /* Compare against expected pubkey from nostrc */
    char *expected_pk = nostr_key_get_public(TEST_SK);
    if (expected_pk && strcmp(pk, expected_pk) != 0) {
        printf("got=%.*s expected=%.*s ", 16, pk, 16, expected_pk);
        free(expected_pk);
        free(pk);
        md_signer_destroy(s);
        FAIL("pubkey mismatch");
        return;
    }
    free(expected_pk);

    free(pk);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: pubkey caching ────────────────────────────────────── */

static void test_pubkey_caching(void) {
    TEST("NIP-55L pubkey caching");

    set_test_key();

    MdSigner *s = md_signer_create_nip55l();
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

/* ── Test: sign_event returns full signed JSON ───────────────── */

static void test_sign_event(void) {
    TEST("NIP-55L sign_event returns full JSON");

    set_test_key();

    MdSigner *s = md_signer_create_nip55l();
    if (!s) { FAIL("create failed"); return; }

    char *pk = NULL;
    md_signer_get_pubkey(s, &pk);
    if (!pk) { md_signer_destroy(s); FAIL("get_pubkey failed"); return; }

    /* Build unsigned event */
    char event_json[512];
    snprintf(event_json, sizeof(event_json),
             "{\"kind\":1,\"content\":\"nip55l test\",\"tags\":[],"
             "\"created_at\":1704067200,\"pubkey\":\"%s\"}", pk);
    free(pk);

    char *signed_json = NULL;
    int ret = md_signer_sign_event(s, event_json, &signed_json);
    if (ret != MD_SIGNER_OK || !signed_json) {
        md_signer_destroy(s);
        FAIL("sign_event failed");
        return;
    }

    /* Must contain sig and id fields (full event, not just signature) */
    if (!strstr(signed_json, "\"sig\"")) {
        free(signed_json);
        md_signer_destroy(s);
        FAIL("missing sig field");
        return;
    }

    if (!strstr(signed_json, "\"id\"")) {
        free(signed_json);
        md_signer_destroy(s);
        FAIL("missing id field");
        return;
    }

    if (!strstr(signed_json, "\"pubkey\"")) {
        free(signed_json);
        md_signer_destroy(s);
        FAIL("missing pubkey field");
        return;
    }

    /* Must contain the content we set */
    if (!strstr(signed_json, "nip55l test")) {
        free(signed_json);
        md_signer_destroy(s);
        FAIL("missing content");
        return;
    }

    free(signed_json);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: NIP-44 encrypt/decrypt roundtrip ──────────────────── */

static void test_nip44_roundtrip(void) {
    TEST("NIP-55L NIP-44 encrypt/decrypt roundtrip");

    /* Use two different keys for sender/receiver */
    static const char *SK2 =
        "b4b147bc522828731f1a016bfa72c073a012fce3c9debc1896eec0da7a5c7d0c";

    /* Sender uses TEST_SK */
    setenv("NOSTR_SIGNER_SECKEY_HEX", TEST_SK, 1);
    MdSigner *sender = md_signer_create_nip55l();
    if (!sender) { FAIL("create sender"); return; }

    /* Get receiver's pubkey */
    char *recv_pk = nostr_key_get_public(SK2);
    char *send_pk = NULL;
    md_signer_get_pubkey(sender, &send_pk);
    if (!recv_pk || !send_pk) {
        free(recv_pk); free(send_pk);
        md_signer_destroy(sender);
        FAIL("get pubkeys");
        return;
    }

    const char *plaintext = "hello from nip55l signer test";

    /* Encrypt */
    char *ciphertext = NULL;
    int ret = md_signer_nip44_encrypt(sender, recv_pk, plaintext, &ciphertext);
    md_signer_destroy(sender);

    if (ret != MD_SIGNER_OK || !ciphertext) {
        free(recv_pk); free(send_pk);
        FAIL("encrypt failed");
        return;
    }

    /* Switch to receiver key and decrypt */
    setenv("NOSTR_SIGNER_SECKEY_HEX", SK2, 1);
    MdSigner *receiver = md_signer_create_nip55l();
    if (!receiver) {
        free(recv_pk); free(send_pk); free(ciphertext);
        FAIL("create receiver");
        return;
    }

    char *decrypted = NULL;
    ret = md_signer_nip44_decrypt(receiver, send_pk, ciphertext, &decrypted);
    md_signer_destroy(receiver);

    if (ret != MD_SIGNER_OK || !decrypted) {
        free(recv_pk); free(send_pk); free(ciphertext);
        FAIL("decrypt failed");
        return;
    }

    if (strcmp(decrypted, plaintext) != 0) {
        printf("expected='%s' got='%s' ", plaintext, decrypted);
        free(recv_pk); free(send_pk); free(ciphertext); free(decrypted);
        FAIL("plaintext mismatch");
        return;
    }

    free(recv_pk);
    free(send_pk);
    free(ciphertext);
    free(decrypted);

    /* Restore test key */
    set_test_key();
    PASS();
}

/* ── Test: stateless — multiple creates share same key ───────── */

static void test_stateless_multiple_creates(void) {
    TEST("NIP-55L stateless multiple creates");

    set_test_key();

    MdSigner *s1 = md_signer_create_nip55l();
    MdSigner *s2 = md_signer_create_nip55l();
    if (!s1 || !s2) {
        md_signer_destroy(s1);
        md_signer_destroy(s2);
        FAIL("create failed");
        return;
    }

    char *pk1 = NULL, *pk2 = NULL;
    md_signer_get_pubkey(s1, &pk1);
    md_signer_get_pubkey(s2, &pk2);

    if (!pk1 || !pk2 || strcmp(pk1, pk2) != 0) {
        free(pk1); free(pk2);
        md_signer_destroy(s1);
        md_signer_destroy(s2);
        FAIL("pubkeys differ across instances");
        return;
    }

    free(pk1);
    free(pk2);
    md_signer_destroy(s1);
    md_signer_destroy(s2);
    PASS();
}

/* ── Test: destroy NULL (should not crash) ───────────────────── */

static void test_destroy_null(void) {
    TEST("NIP-55L destroy NULL safe");

    md_signer_destroy(NULL);
    PASS();
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_signer_nip55l:\n");

    test_no_key_fails();
    test_type_name();
    test_lifecycle();
    test_get_pubkey_hex();
    test_pubkey_caching();
    test_sign_event();
    test_nip44_roundtrip();
    test_stateless_multiple_creates();
    test_destroy_null();

    /* Clean up env */
    clear_test_key();

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
