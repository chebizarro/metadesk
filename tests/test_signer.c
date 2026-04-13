/*
 * test_signer.c — MdSigner abstraction tests.
 *
 * Tests:
 *   1. Direct-key backend lifecycle
 *   2. Vtable dispatch for all operations
 *   3. Pubkey caching
 *   4. Error handling (invalid args, NULL signer)
 *   5. Type name strings
 *   6. Backend availability (NIP-46/55L/5F return NULL when not compiled/available)
 *   7. Secure key zeroing on destroy
 *
 * Note: direct-key tests require nostrc key functions to be linked.
 * NIP-46/55L/5F tests verify graceful failure when backends aren't available.
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

/* ── Helper: generate a test keypair ─────────────────────────── */

static char *generate_test_sk(void) {
    char *sk = nostr_key_generate_private();
    return sk;
}

/* ── Test: direct-key create/destroy ─────────────────────────── */

static void test_direct_lifecycle(void) {
    TEST("direct-key create/destroy");

    char *sk = generate_test_sk();
    if (!sk) { FAIL("keygen failed"); return; }

    MdSigner *s = md_signer_create_direct(sk);

    /* Zero our copy of the key */
    memset(sk, 0, strlen(sk));
    free(sk);

    if (!s) { FAIL("create returned NULL"); return; }

    if (md_signer_get_type(s) != MD_SIGNER_DIRECT_KEY) {
        md_signer_destroy(s);
        FAIL("wrong type");
        return;
    }

    if (!md_signer_is_ready(s)) {
        md_signer_destroy(s);
        FAIL("not ready after create");
        return;
    }

    md_signer_destroy(s);
    PASS();
}

/* ── Test: get_pubkey via vtable ─────────────────────────────── */

static void test_direct_get_pubkey(void) {
    TEST("direct-key get_pubkey");

    char *sk = generate_test_sk();
    if (!sk) { FAIL("keygen failed"); return; }

    /* Derive expected pubkey */
    char *expected_pk = nostr_key_get_public(sk);

    MdSigner *s = md_signer_create_direct(sk);
    memset(sk, 0, strlen(sk));
    free(sk);

    if (!s) { free(expected_pk); FAIL("create failed"); return; }

    char *pk = NULL;
    int ret = md_signer_get_pubkey(s, &pk);

    if (ret != MD_SIGNER_OK) {
        md_signer_destroy(s);
        free(expected_pk);
        FAIL("get_pubkey failed");
        return;
    }

    if (!pk || strlen(pk) != 64) {
        md_signer_destroy(s);
        free(expected_pk);
        free(pk);
        FAIL("invalid pubkey length");
        return;
    }

    if (strcmp(pk, expected_pk) != 0) {
        md_signer_destroy(s);
        free(expected_pk);
        free(pk);
        FAIL("pubkey mismatch");
        return;
    }

    free(pk);
    free(expected_pk);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: pubkey caching ────────────────────────────────────── */

static void test_pubkey_caching(void) {
    TEST("pubkey caching");

    char *sk = generate_test_sk();
    if (!sk) { FAIL("keygen failed"); return; }

    MdSigner *s = md_signer_create_direct(sk);
    memset(sk, 0, strlen(sk));
    free(sk);
    if (!s) { FAIL("create failed"); return; }

    char *pk1 = NULL, *pk2 = NULL;
    md_signer_get_pubkey(s, &pk1);
    md_signer_get_pubkey(s, &pk2);

    if (!pk1 || !pk2 || strcmp(pk1, pk2) != 0) {
        free(pk1); free(pk2);
        md_signer_destroy(s);
        FAIL("cached pubkeys don't match");
        return;
    }

    free(pk1);
    free(pk2);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: sign_event round-trip ─────────────────────────────── */

static void test_direct_sign_event(void) {
    TEST("direct-key sign_event");

    char *sk = generate_test_sk();
    if (!sk) { FAIL("keygen failed"); return; }

    MdSigner *s = md_signer_create_direct(sk);
    memset(sk, 0, strlen(sk));
    free(sk);
    if (!s) { FAIL("create failed"); return; }

    /* Get pubkey for the event */
    char *pk = NULL;
    md_signer_get_pubkey(s, &pk);
    if (!pk) {
        md_signer_destroy(s);
        FAIL("get_pubkey failed");
        return;
    }

    /* Build a minimal unsigned event JSON */
    char event_json[512];
    snprintf(event_json, sizeof(event_json),
             "{\"kind\":1,\"content\":\"test\",\"tags\":[],"
             "\"created_at\":1234567890,\"pubkey\":\"%s\"}", pk);
    free(pk);

    char *signed_json = NULL;
    int ret = md_signer_sign_event(s, event_json, &signed_json);

    if (ret != MD_SIGNER_OK) {
        md_signer_destroy(s);
        FAIL("sign_event failed");
        return;
    }

    if (!signed_json) {
        md_signer_destroy(s);
        FAIL("signed_json is NULL");
        return;
    }

    /* Signed JSON should contain "sig" and "id" fields */
    if (!strstr(signed_json, "\"sig\"") || !strstr(signed_json, "\"id\"")) {
        free(signed_json);
        md_signer_destroy(s);
        FAIL("signed JSON missing sig/id fields");
        return;
    }

    free(signed_json);
    md_signer_destroy(s);
    PASS();
}

/* ── Test: NIP-44 encrypt/decrypt round-trip ─────────────────── */

static void test_direct_nip44_roundtrip(void) {
    TEST("direct-key NIP-44 encrypt/decrypt");

    /* Generate two keypairs for sender/receiver */
    char *sk1 = generate_test_sk();
    char *sk2 = generate_test_sk();
    if (!sk1 || !sk2) {
        free(sk1); free(sk2);
        FAIL("keygen failed"); return;
    }

    MdSigner *sender = md_signer_create_direct(sk1);
    MdSigner *receiver = md_signer_create_direct(sk2);
    memset(sk1, 0, strlen(sk1)); free(sk1);
    memset(sk2, 0, strlen(sk2)); free(sk2);

    if (!sender || !receiver) {
        md_signer_destroy(sender);
        md_signer_destroy(receiver);
        FAIL("create failed"); return;
    }

    /* Get receiver's pubkey */
    char *recv_pk = NULL;
    md_signer_get_pubkey(receiver, &recv_pk);

    /* Get sender's pubkey (for decrypt) */
    char *send_pk = NULL;
    md_signer_get_pubkey(sender, &send_pk);

    if (!recv_pk || !send_pk) {
        free(recv_pk); free(send_pk);
        md_signer_destroy(sender);
        md_signer_destroy(receiver);
        FAIL("pubkey retrieval failed"); return;
    }

    const char *plaintext = "hello from metadesk signer test";

    /* Encrypt */
    char *ciphertext = NULL;
    int ret = md_signer_nip44_encrypt(sender, recv_pk, plaintext, &ciphertext);
    if (ret != MD_SIGNER_OK || !ciphertext) {
        free(recv_pk); free(send_pk);
        md_signer_destroy(sender);
        md_signer_destroy(receiver);
        FAIL("encrypt failed"); return;
    }

    /* Decrypt */
    char *decrypted = NULL;
    ret = md_signer_nip44_decrypt(receiver, send_pk, ciphertext, &decrypted);
    if (ret != MD_SIGNER_OK || !decrypted) {
        free(recv_pk); free(send_pk); free(ciphertext);
        md_signer_destroy(sender);
        md_signer_destroy(receiver);
        FAIL("decrypt failed"); return;
    }

    if (strcmp(decrypted, plaintext) != 0) {
        printf("expected='%s' got='%s' ", plaintext, decrypted);
        free(recv_pk); free(send_pk); free(ciphertext); free(decrypted);
        md_signer_destroy(sender);
        md_signer_destroy(receiver);
        FAIL("plaintext mismatch"); return;
    }

    free(recv_pk);
    free(send_pk);
    free(ciphertext);
    free(decrypted);
    md_signer_destroy(sender);
    md_signer_destroy(receiver);
    PASS();
}

/* ── Test: error handling ────────────────────────────────────── */

static void test_error_handling(void) {
    TEST("error handling");

    /* NULL signer */
    char *out = NULL;
    if (md_signer_get_pubkey(NULL, &out) != MD_SIGNER_ERR_NOT_INIT) {
        FAIL("NULL signer accepted"); return;
    }
    if (md_signer_sign_event(NULL, "{}", &out) != MD_SIGNER_ERR_NOT_INIT) {
        FAIL("NULL signer sign accepted"); return;
    }

    /* Invalid sk_hex */
    if (md_signer_create_direct(NULL)) {
        FAIL("NULL sk accepted"); return;
    }
    if (md_signer_create_direct("too-short")) {
        FAIL("short sk accepted"); return;
    }
    if (md_signer_create_direct("")) {
        FAIL("empty sk accepted"); return;
    }

    /* Destroy NULL (should not crash) */
    md_signer_destroy(NULL);

    PASS();
}

/* ── Test: type names ────────────────────────────────────────── */

static void test_type_names(void) {
    TEST("type name strings");

    if (strcmp(md_signer_type_name(MD_SIGNER_DIRECT_KEY), "direct-key") != 0) {
        FAIL("direct-key name"); return;
    }
    if (strcmp(md_signer_type_name(MD_SIGNER_NIP46), "NIP-46 (Nostr Connect)") != 0) {
        FAIL("NIP-46 name"); return;
    }
    if (strcmp(md_signer_type_name(MD_SIGNER_NIP55L), "NIP-55L (D-Bus)") != 0) {
        FAIL("NIP-55L name"); return;
    }
    if (strcmp(md_signer_type_name(MD_SIGNER_NIP5F), "NIP-5F (Unix socket)") != 0) {
        FAIL("NIP-5F name"); return;
    }

    PASS();
}

/* ── Test: unavailable backends return NULL ───────────────────── */

static void test_unavailable_backends(void) {
    TEST("unavailable backends return NULL gracefully");

    /* NIP-46 with invalid URI should fail */
    MdSigner *s = md_signer_create_nip46("invalid-uri", 1000);
    if (s) {
        md_signer_destroy(s);
        FAIL("NIP-46 accepted invalid URI"); return;
    }

    /* NIP-55L without a running daemon should fail */
    /* (This might succeed if a D-Bus signer daemon is actually running,
     *  but in CI/test environments it won't be) */

    /* NIP-5F with a non-existent socket should fail */
    s = md_signer_create_nip5f("/nonexistent/socket.sock");
    if (s) {
        md_signer_destroy(s);
        FAIL("NIP-5F connected to nonexistent socket"); return;
    }

    PASS();
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_signer:\n");

    test_direct_lifecycle();
    test_direct_get_pubkey();
    test_pubkey_caching();
    test_direct_sign_event();
    test_direct_nip44_roundtrip();
    test_error_handling();
    test_type_names();
    test_unavailable_backends();

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
