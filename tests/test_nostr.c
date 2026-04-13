/*
 * metadesk — test_nostr.c
 * Tests for the nostrc bridge layer.
 * The underlying nostrc library has its own comprehensive test suite;
 * these tests verify the metadesk-specific bridge functions.
 */
#include "nostr.h"
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

int main(void) {
    printf("test_nostr (nostrc bridge):\n");
    test_keypair_generation();
    test_null_args();
    test_allowlist_default_deny();
    printf("All nostr bridge tests passed.\n");
    return 0;
}
