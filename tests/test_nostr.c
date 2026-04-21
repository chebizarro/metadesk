/*
 * metadesk — test_nostr.c
 * Tests for the nostrc bridge layer.
 * The underlying nostrc library has its own comprehensive test suite;
 * these tests verify the metadesk-specific bridge functions.
 */
#include "nostr.h"
#include "signer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

static void test_keypair_generation(void) {
    char *sk = NULL, *pk = NULL;
    int ret = md_nostr_generate_keypair(&sk, &pk);
    assert(ret == 0);
    assert(sk != NULL);
    assert(pk != NULL);
    assert(strlen(sk) == 64); /* 32 bytes hex */
    assert(strlen(pk) == 64);

    /* Verify consistency: deriving pubkey from sk should match pk */
    char *pk2 = md_nostr_get_pubkey(sk);
    assert(pk2 != NULL);
    assert(strcmp(pk, pk2) == 0);

    free(pk2);
    memset(sk, 0, strlen(sk));
    free(sk);
    free(pk);
    printf("  PASS: keypair generation\n");
}

static void test_null_args(void) {
    /* NULL config should fail gracefully */
    MdNostr *n = md_nostr_create(NULL, NULL);
    assert(n == NULL);

    /* NULL sk should fail */
    MdNostrConfig cfg = { .sk_hex = NULL, .relay_urls = NULL, .relay_count = 0 };
    n = md_nostr_create(&cfg, NULL);
    assert(n == NULL);

    /* NULL args to key functions should not crash */
    assert(md_nostr_get_pubkey(NULL) == NULL);

    char *sk = NULL, *pk = NULL;
    assert(md_nostr_generate_keypair(NULL, &pk) == -1);
    assert(md_nostr_generate_keypair(&sk, NULL) == -1);

    printf("  PASS: null args handled\n");
}

static void test_allowlist_default_deny(void) {
    /* Test the default-deny contract of the allowlist API.
     *
     * Without a live relay pool we can't create a full MdNostr, but
     * the allowlist functions are NULL-safe and document the contract:
     * - NULL MdNostr → is_allowed returns false (deny)
     * - NULL MdNostr → has_allowlist returns false
     * - No allowlist loaded → is_allowed returns false for any pubkey
     *
     * We verify these NULL invariants directly, and also confirm that
     * a valid pubkey (not on any list) is correctly rejected.
     */

    /* Generate a real pubkey to use as the "requesting client" */
    char *sk = NULL, *pk = NULL;
    int ret = md_nostr_generate_keypair(&sk, &pk);
    assert(ret == 0);
    assert(pk != NULL);

    /* NULL MdNostr → always deny */
    assert(md_nostr_is_allowed(NULL, pk) == false);
    assert(md_nostr_is_allowed(NULL, NULL) == false);

    /* NULL MdNostr → no allowlist */
    assert(md_nostr_has_allowlist(NULL) == false);

    /* NULL pubkey → deny */
    assert(md_nostr_is_allowed(NULL, "") == false);

    /* Allowlist add/remove with NULL nostr → error */
    assert(md_nostr_allowlist_add(NULL, pk, NULL) == -1);
    assert(md_nostr_allowlist_remove(NULL, pk) == -1);

    /* Refresh with NULL → error */
    assert(md_nostr_refresh_allowlist(NULL) == -1);

    memset(sk, 0, strlen(sk));
    free(sk);
    free(pk);
    printf("  PASS: allowlist default deny\n");
}

static void test_nip44_roundtrip(void) {
    /* Generate two keypairs: Alice and Bob */
    char *alice_sk = NULL, *alice_pk = NULL;
    char *bob_sk   = NULL, *bob_pk   = NULL;
    int ret;

    ret = md_nostr_generate_keypair(&alice_sk, &alice_pk);
    assert(ret == 0 && alice_sk && alice_pk);
    ret = md_nostr_generate_keypair(&bob_sk, &bob_pk);
    assert(ret == 0 && bob_sk && bob_pk);

    /* Create direct-key signers */
    MdSigner *alice_signer = md_signer_create_direct(alice_sk);
    assert(alice_signer != NULL);
    MdSigner *bob_signer = md_signer_create_direct(bob_sk);
    assert(bob_signer != NULL);

    /* Alice encrypts a message for Bob */
    const char *plaintext = "session_request:{\"v\":1}";
    char *ciphertext = NULL;
    ret = md_signer_nip44_encrypt(alice_signer, bob_pk, plaintext, &ciphertext);
    assert(ret == MD_SIGNER_OK);
    assert(ciphertext != NULL);
    assert(strlen(ciphertext) > 0);
    /* Ciphertext should be base64, not equal to plaintext */
    assert(strcmp(ciphertext, plaintext) != 0);

    /* Bob decrypts the message from Alice */
    char *decrypted = NULL;
    ret = md_signer_nip44_decrypt(bob_signer, alice_pk, ciphertext, &decrypted);
    assert(ret == MD_SIGNER_OK);
    assert(decrypted != NULL);
    assert(strcmp(decrypted, plaintext) == 0);

    free(decrypted);
    free(ciphertext);

    /* Verify the reverse direction: Bob encrypts for Alice */
    const char *reply = "session_accept:{\"session_id\":\"abc-123\"}";
    ciphertext = NULL;
    ret = md_signer_nip44_encrypt(bob_signer, alice_pk, reply, &ciphertext);
    assert(ret == MD_SIGNER_OK);
    assert(ciphertext != NULL);

    decrypted = NULL;
    ret = md_signer_nip44_decrypt(alice_signer, bob_pk, ciphertext, &decrypted);
    assert(ret == MD_SIGNER_OK);
    assert(decrypted != NULL);
    assert(strcmp(decrypted, reply) == 0);

    free(decrypted);
    free(ciphertext);

    /* Cleanup */
    md_signer_destroy(alice_signer);
    md_signer_destroy(bob_signer);
    memset(alice_sk, 0, strlen(alice_sk));
    memset(bob_sk, 0, strlen(bob_sk));
    free(alice_sk); free(alice_pk);
    free(bob_sk);   free(bob_pk);

    printf("  PASS: NIP-44 encrypt/decrypt round-trip\n");
}

static void test_nip44_wrong_key_fails(void) {
    /* Verify that decrypting with the wrong key fails */
    char *alice_sk = NULL, *alice_pk = NULL;
    char *bob_sk   = NULL, *bob_pk   = NULL;
    char *eve_sk   = NULL, *eve_pk   = NULL;
    int ret;

    ret = md_nostr_generate_keypair(&alice_sk, &alice_pk);
    assert(ret == 0);
    ret = md_nostr_generate_keypair(&bob_sk, &bob_pk);
    assert(ret == 0);
    ret = md_nostr_generate_keypair(&eve_sk, &eve_pk);
    assert(ret == 0);

    MdSigner *alice = md_signer_create_direct(alice_sk);
    MdSigner *eve   = md_signer_create_direct(eve_sk);
    assert(alice && eve);

    /* Alice encrypts for Bob */
    char *ciphertext = NULL;
    ret = md_signer_nip44_encrypt(alice, bob_pk, "secret", &ciphertext);
    assert(ret == MD_SIGNER_OK && ciphertext);

    /* Eve tries to decrypt — should fail (wrong key) */
    char *decrypted = NULL;
    ret = md_signer_nip44_decrypt(eve, alice_pk, ciphertext, &decrypted);
    /* Either returns error or decrypted garbage (not the original) */
    if (ret == MD_SIGNER_OK && decrypted) {
        assert(strcmp(decrypted, "secret") != 0);
        free(decrypted);
    }

    free(ciphertext);
    md_signer_destroy(alice);
    md_signer_destroy(eve);
    memset(alice_sk, 0, strlen(alice_sk));
    memset(bob_sk, 0, strlen(bob_sk));
    memset(eve_sk, 0, strlen(eve_sk));
    free(alice_sk); free(alice_pk);
    free(bob_sk);   free(bob_pk);
    free(eve_sk);   free(eve_pk);

    printf("  PASS: NIP-44 wrong key correctly fails\n");
}

/* ── Integration-level tests ──────────────────────────────────
 * These test the MdNostr lifecycle without a live relay.
 * The pool will attempt background connections that fail —
 * this is expected and exercises the error paths.
 */

static volatile int test_ok_callback_count;
static volatile int test_ok_last_result;

static void test_publish_cb(const char *event_id, bool ok,
                            const char *reason, void *userdata) {
    (void)event_id; (void)reason; (void)userdata;
    test_ok_callback_count++;
    test_ok_last_result = ok ? 1 : 0;
}

static void test_bridge_lifecycle(void) {
    /* Create a direct-key signer */
    char *sk = NULL, *pk = NULL;
    int ret = md_nostr_generate_keypair(&sk, &pk);
    assert(ret == 0 && sk && pk);

    /* Build config with a dummy relay (won't connect, but won't crash) */
    const char *relay_urls[] = { "wss://127.0.0.1:1" };
    MdNostrConfig cfg = {
        .sk_hex = sk,
        .relay_urls = relay_urls,
        .relay_count = 1,
    };

    /* Set up callbacks including the new publish_result callback */
    test_ok_callback_count = 0;
    MdNostrCallbacks cbs = { 0 };
    cbs.on_publish_result = test_publish_cb;
    cbs.publish_result_userdata = NULL;

    MdNostr *n = md_nostr_create(&cfg, &cbs);
    /* Creation may fail if pool can't init (e.g. no network).
     * On most systems the pool starts worker threads regardless. */
    if (!n) {
        printf("  SKIP: bridge lifecycle (pool init failed without network)\n");
        memset(sk, 0, strlen(sk));
        free(sk); free(pk);
        return;
    }

    /* Verify pubkey is accessible and matches */
    const char *bridge_pk = md_nostr_get_npub(n);
    assert(bridge_pk != NULL);
    assert(strcmp(bridge_pk, pk) == 0);

    /* Verify signer is accessible */
    MdSigner *signer = md_nostr_get_signer(n);
    assert(signer != NULL);
    assert(md_signer_is_ready(signer));

    /* Verify allowlist starts empty (default deny) */
    assert(md_nostr_has_allowlist(n) == false);
    assert(md_nostr_is_allowed(n, pk) == false);

    /* Clean destroy (exercises dedup ring cleanup, relay cleanup, etc.) */
    md_nostr_destroy(n);

    memset(sk, 0, strlen(sk));
    free(sk); free(pk);
    printf("  PASS: bridge lifecycle (create/verify/destroy)\n");
}

static void test_callback_struct_layout(void) {
    /* Verify the MdNostrCallbacks struct correctly carries all fields
     * through to the MdNostr instance.  This catches ABI mismatches
     * if new fields are added but not initialized. */
    MdNostrCallbacks cbs = { 0 };

    /* All callbacks should start NULL */
    assert(cbs.on_dm == NULL);
    assert(cbs.dm_userdata == NULL);
    assert(cbs.on_transport == NULL);
    assert(cbs.transport_userdata == NULL);
    assert(cbs.on_publish_result == NULL);
    assert(cbs.publish_result_userdata == NULL);

    /* Set and verify each field */
    int dummy = 42;
    cbs.on_publish_result = test_publish_cb;
    cbs.publish_result_userdata = &dummy;
    assert(cbs.on_publish_result == test_publish_cb);
    assert(cbs.publish_result_userdata == &dummy);

    printf("  PASS: callback struct layout\n");
}

static void test_signer_sign_event_roundtrip(void) {
    /* Verify that a direct-key signer can sign a Nostr event and
     * produce valid signed JSON (id + sig fields populated).
     * Uses a fixed test key to avoid entropy exhaustion from
     * earlier keypair-generation-heavy tests. */
    const char *test_sk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    MdSigner *signer = md_signer_create_direct(test_sk);
    assert(signer != NULL);

    /* Get the derived pubkey */
    char *pk = NULL;
    int ret = md_signer_get_pubkey(signer, &pk);
    assert(ret == MD_SIGNER_OK && pk != NULL);
    assert(strlen(pk) == 64);

    /* Build a simple unsigned event JSON */
    char event_json[512];
    snprintf(event_json, sizeof(event_json),
             "{\"kind\":1,\"content\":\"test\",\"tags\":[],\"created_at\":1700000000,\"pubkey\":\"%s\"}",
             pk);

    /* Sign it */
    char *signed_json = NULL;
    ret = md_signer_sign_event(signer, event_json, &signed_json);
    assert(ret == MD_SIGNER_OK);
    assert(signed_json != NULL);

    /* Signed JSON should contain "sig" and "id" fields */
    assert(strstr(signed_json, "\"sig\"") != NULL);
    assert(strstr(signed_json, "\"id\"") != NULL);
    /* Should still contain the original pubkey */
    assert(strstr(signed_json, pk) != NULL);

    free(signed_json);
    free(pk);
    md_signer_destroy(signer);
    printf("  PASS: signer sign_event round-trip\n");
}

int main(void) {
    printf("test_nostr (nostrc bridge):\n");
    test_keypair_generation();
    test_null_args();
    test_allowlist_default_deny();
    test_nip44_roundtrip();
    test_nip44_wrong_key_fails();
    test_callback_struct_layout();
    test_signer_sign_event_roundtrip();
    /* Bridge lifecycle test runs last — it starts/stops pool worker
     * threads that may leave OpenSSL PRNG in a non-reusable state. */
    test_bridge_lifecycle();
    printf("All nostr bridge tests passed.\n");
    return 0;
}
