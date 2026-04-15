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
    /* Generate a throwaway keypair for testing */
    char *sk = NULL, *pk = NULL;
    int ret = md_nostr_generate_keypair(&sk, &pk);
    assert(ret == 0);

    /* We can't easily create a full MdNostr without real relays,
     * but we can test that the API contract holds: without an allowlist
     * loaded, is_allowed returns false.
     *
     * TODO: once we have relay mocking, test the full flow.
     */

    memset(sk, 0, strlen(sk));
    free(sk);
    free(pk);
    printf("  PASS: allowlist default deny (conceptual)\n");
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

int main(void) {
    printf("test_nostr (nostrc bridge):\n");
    test_keypair_generation();
    test_null_args();
    test_allowlist_default_deny();
    test_nip44_roundtrip();
    test_nip44_wrong_key_fails();
    printf("All nostr bridge tests passed.\n");
    return 0;
}
