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
#include <stdbool.h>
#include "md_test.h"
#include <nostr-kinds.h>
#include <json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static void test_keypair_generation(void) {
    char *sk = NULL, *pk = NULL;
    int ret = md_nostr_generate_keypair(&sk, &pk);
    MD_CHECK(ret == 0);
    MD_CHECK(sk != NULL);
    MD_CHECK(pk != NULL);
    MD_CHECK(strlen(sk) == 64); /* 32 bytes hex */
    MD_CHECK(strlen(pk) == 64);

    /* Verify consistency: deriving pubkey from sk should match pk */
    char *pk2 = md_nostr_get_pubkey(sk);
    MD_CHECK(pk2 != NULL);
    MD_CHECK(strcmp(pk, pk2) == 0);

    free(pk2);
    memset(sk, 0, strlen(sk));
    free(sk);
    free(pk);
    printf("  PASS: keypair generation\n");
}

static void test_null_args(void) {
    /* NULL config should fail gracefully */
    MdNostr *n = md_nostr_create(NULL, NULL);
    MD_CHECK(n == NULL);

    /* NULL sk should fail */
    MdNostrConfig cfg = { .sk_hex = NULL, .relay_urls = NULL, .relay_count = 0 };
    n = md_nostr_create(&cfg, NULL);
    MD_CHECK(n == NULL);

    /* NULL args to key functions should not crash */
    MD_CHECK(md_nostr_get_pubkey(NULL) == NULL);

    char *sk = NULL, *pk = NULL;
    MD_CHECK(md_nostr_generate_keypair(NULL, &pk) == -1);
    MD_CHECK(md_nostr_generate_keypair(&sk, NULL) == -1);

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
    MD_CHECK(ret == 0);
    MD_CHECK(pk != NULL);

    /* NULL MdNostr → always deny */
    MD_CHECK(md_nostr_is_allowed(NULL, pk) == false);
    MD_CHECK(md_nostr_is_allowed(NULL, NULL) == false);

    /* NULL MdNostr → no allowlist */
    MD_CHECK(md_nostr_has_allowlist(NULL) == false);

    /* NULL pubkey → deny */
    MD_CHECK(md_nostr_is_allowed(NULL, "") == false);

    /* Allowlist add/remove with NULL nostr → error */
    MD_CHECK(md_nostr_allowlist_add(NULL, pk, NULL) == -1);
    MD_CHECK(md_nostr_allowlist_remove(NULL, pk) == -1);

    /* Refresh with NULL → error */
    MD_CHECK(md_nostr_refresh_allowlist(NULL) == -1);

    /* Accessor functions with NULL nostr → safe defaults */
    MD_CHECK(md_nostr_allowlist_count(NULL) == 0);

    MdAllowlistEntry entry_out;
    MD_CHECK(md_nostr_allowlist_get_entry(NULL, 0, &entry_out) == -1);

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
    MD_CHECK(ret == 0 && alice_sk && alice_pk);
    ret = md_nostr_generate_keypair(&bob_sk, &bob_pk);
    MD_CHECK(ret == 0 && bob_sk && bob_pk);

    /* Create direct-key signers */
    MdSigner *alice_signer = md_signer_create_direct(alice_sk);
    MD_CHECK(alice_signer != NULL);
    MdSigner *bob_signer = md_signer_create_direct(bob_sk);
    MD_CHECK(bob_signer != NULL);

    /* Alice encrypts a message for Bob */
    const char *plaintext = "session_request:{\"v\":1}";
    char *ciphertext = NULL;
    ret = md_signer_nip44_encrypt(alice_signer, bob_pk, plaintext, &ciphertext);
    MD_CHECK(ret == MD_SIGNER_OK);
    MD_CHECK(ciphertext != NULL);
    MD_CHECK(strlen(ciphertext) > 0);
    /* Ciphertext should be base64, not equal to plaintext */
    MD_CHECK(strcmp(ciphertext, plaintext) != 0);

    /* Bob decrypts the message from Alice */
    char *decrypted = NULL;
    ret = md_signer_nip44_decrypt(bob_signer, alice_pk, ciphertext, &decrypted);
    MD_CHECK(ret == MD_SIGNER_OK);
    MD_CHECK(decrypted != NULL);
    MD_CHECK(strcmp(decrypted, plaintext) == 0);

    free(decrypted);
    free(ciphertext);

    /* Verify the reverse direction: Bob encrypts for Alice */
    const char *reply = "session_accept:{\"session_id\":\"abc-123\"}";
    ciphertext = NULL;
    ret = md_signer_nip44_encrypt(bob_signer, alice_pk, reply, &ciphertext);
    MD_CHECK(ret == MD_SIGNER_OK);
    MD_CHECK(ciphertext != NULL);

    decrypted = NULL;
    ret = md_signer_nip44_decrypt(alice_signer, bob_pk, ciphertext, &decrypted);
    MD_CHECK(ret == MD_SIGNER_OK);
    MD_CHECK(decrypted != NULL);
    MD_CHECK(strcmp(decrypted, reply) == 0);

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
    MD_CHECK(ret == 0);
    ret = md_nostr_generate_keypair(&bob_sk, &bob_pk);
    MD_CHECK(ret == 0);
    ret = md_nostr_generate_keypair(&eve_sk, &eve_pk);
    MD_CHECK(ret == 0);

    MdSigner *alice = md_signer_create_direct(alice_sk);
    MdSigner *eve   = md_signer_create_direct(eve_sk);
    MD_CHECK(alice && eve);

    /* Alice encrypts for Bob */
    char *ciphertext = NULL;
    ret = md_signer_nip44_encrypt(alice, bob_pk, "secret", &ciphertext);
    MD_CHECK(ret == MD_SIGNER_OK && ciphertext);

    /* Eve tries to decrypt — should fail (wrong key) */
    char *decrypted = NULL;
    ret = md_signer_nip44_decrypt(eve, alice_pk, ciphertext, &decrypted);
    /* Either returns error or decrypted garbage (not the original) */
    if (ret == MD_SIGNER_OK && decrypted) {
        MD_CHECK(strcmp(decrypted, "secret") != 0);
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
    MD_CHECK(ret == 0 && sk && pk);

    /* Build config with dummy relays (won't connect, but won't crash).
     * Multiple URLs exercise relay snapshot allocation/free loops. */
    const char *relay_urls[] = { "wss://127.0.0.1:1", "wss://127.0.0.1:2" };
    MdNostrConfig cfg = {
        .sk_hex = sk,
        .relay_urls = relay_urls,
        .relay_count = 2,
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
    MD_CHECK(bridge_pk != NULL);
    MD_CHECK(strcmp(bridge_pk, pk) == 0);

    /* Verify signer is accessible */
    MdSigner *signer = md_nostr_get_signer(n);
    MD_CHECK(signer != NULL);
    MD_CHECK(md_signer_is_ready(signer));

    /* Verify allowlist starts empty (default deny) */
    MD_CHECK(md_nostr_has_allowlist(n) == false);
    MD_CHECK(md_nostr_is_allowed(n, pk) == false);

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
    MD_CHECK(cbs.on_dm == NULL);
    MD_CHECK(cbs.dm_userdata == NULL);
    MD_CHECK(cbs.on_publish_result == NULL);
    MD_CHECK(cbs.publish_result_userdata == NULL);

    /* Set and verify each field */
    int dummy = 42;
    cbs.on_publish_result = test_publish_cb;
    cbs.publish_result_userdata = &dummy;
    MD_CHECK(cbs.on_publish_result == test_publish_cb);
    MD_CHECK(cbs.publish_result_userdata == &dummy);

    printf("  PASS: callback struct layout\n");
}

static void test_signer_sign_event_roundtrip(void) {
    /* Verify that a direct-key signer can sign a Nostr event and
     * produce valid signed JSON (id + sig fields populated).
     * Uses a fixed test key to avoid entropy exhaustion from
     * earlier keypair-generation-heavy tests. */
    const char *test_sk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    MdSigner *signer = md_signer_create_direct(test_sk);
    MD_CHECK(signer != NULL);

    /* Get the derived pubkey */
    char *pk = NULL;
    int ret = md_signer_get_pubkey(signer, &pk);
    MD_CHECK(ret == MD_SIGNER_OK && pk != NULL);
    MD_CHECK(strlen(pk) == 64);

    /* Build a simple unsigned event JSON */
    char event_json[512];
    snprintf(event_json, sizeof(event_json),
             "{\"kind\":1,\"content\":\"test\",\"tags\":[],\"created_at\":1700000000,\"pubkey\":\"%s\"}",
             pk);

    /* Sign it */
    char *signed_json = NULL;
    ret = md_signer_sign_event(signer, event_json, &signed_json);
    MD_CHECK(ret == MD_SIGNER_OK);
    MD_CHECK(signed_json != NULL);

    /* Signed JSON should contain "sig" and "id" fields */
    MD_CHECK(strstr(signed_json, "\"sig\"") != NULL);
    MD_CHECK(strstr(signed_json, "\"id\"") != NULL);
    /* Should still contain the original pubkey */
    MD_CHECK(strstr(signed_json, pk) != NULL);

    free(signed_json);
    free(pk);
    md_signer_destroy(signer);
    printf("  PASS: signer sign_event round-trip\n");
}

typedef enum {
    DM_GOOD, DM_WRAP_SIG, DM_WRAP_ID, DM_WRAP_RECIPIENT,
    DM_SEAL_SIG, DM_SEAL_KIND, DM_SEAL_TAGS, DM_RUMOR_KIND,
    DM_RUMOR_ID, DM_RUMOR_SIGNED, DM_SENDER, DM_RUMOR_RECIPIENT,
    DM_SEAL_JSON, DM_RUMOR_JSON
} DmFixture;
static const char *dm_sender_sk =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char *dm_recipient_sk =
    "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
static const char *dm_ephemeral_sk =
    "1111111111111111111111111111111111111111111111111111111111111111";

static NostrEvent *dm_fixture(DmFixture fixture, const char *content) {
    MdSigner *sender = md_signer_create_direct(dm_sender_sk);
    MdSigner *ephemeral = md_signer_create_direct(dm_ephemeral_sk);
    char *pk = md_nostr_get_pubkey(dm_recipient_sk);
    char *sender_pk = md_nostr_get_pubkey(dm_sender_sk);
    char *ephemeral_pk = md_nostr_get_pubkey(dm_ephemeral_sk);
    NostrEvent *rumor = nostr_nip17_create_rumor(
        fixture == DM_SENDER ? pk : sender_pk,
        fixture == DM_RUMOR_RECIPIENT ? sender_pk : pk, content, 1700000000);
    if (fixture == DM_RUMOR_KIND) nostr_event_set_kind(rumor, NOSTR_KIND_TEXT_NOTE);
    char id[65];
    MD_CHECK(nostr_event_compute_id(rumor, id) == NOSTR_EVENT_VALIDATION_OK);
    rumor->id = strdup(id);
    if (fixture == DM_RUMOR_ID) rumor->id[0] = rumor->id[0] == 'a' ? 'b' : 'a';
    if (fixture == DM_RUMOR_SIGNED) MD_CHECK(nostr_event_sign(rumor, dm_sender_sk) == 0);
    char *rumor_json = nostr_event_serialize(rumor);
    nostr_event_free(rumor);
    char *ct = NULL;
    MD_CHECK(md_signer_nip44_encrypt(sender, pk,
        fixture == DM_RUMOR_JSON ? "not-json" : rumor_json, &ct) == 0);
    free(rumor_json);
    NostrEvent *seal = nostr_event_new();
    nostr_event_set_kind(seal, fixture == DM_SEAL_KIND ? NOSTR_KIND_TEXT_NOTE : NOSTR_KIND_SEAL);
    nostr_event_set_pubkey(seal, sender_pk);
    nostr_event_set_content(seal, ct); free(ct); ct = NULL;
    /* Near the oldest valid NIP-59 timestamp, not a five-minute freshness test. */
    nostr_event_set_created_at(seal, (int64_t)time(NULL) - 2 * 86400 + 10);
    nostr_event_set_tags(seal, fixture == DM_SEAL_TAGS ?
        nostr_tags_new(1, nostr_tag_new("p", pk, NULL)) : nostr_tags_new(0));
    MD_CHECK(nostr_event_sign(seal, dm_sender_sk) == 0);
    if (fixture == DM_SEAL_SIG) seal->sig[0] = seal->sig[0] == 'a' ? 'b' : 'a';
    char *seal_json = nostr_event_serialize(seal);
    nostr_event_free(seal);
    MD_CHECK(md_signer_nip44_encrypt(ephemeral, pk,
        fixture == DM_SEAL_JSON ? "not-json" : seal_json, &ct) == 0);
    free(seal_json);
    NostrEvent *wrap = nostr_event_new();
    nostr_event_set_kind(wrap, NOSTR_KIND_GIFT_WRAP);
    nostr_event_set_pubkey(wrap, ephemeral_pk);
    nostr_event_set_content(wrap, ct); free(ct);
    nostr_event_set_created_at(wrap, (int64_t)time(NULL) - 2 * 86400 + 10);
    nostr_event_set_tags(wrap, nostr_tags_new(1, nostr_tag_new("p",
        fixture == DM_WRAP_RECIPIENT ? sender_pk : pk, NULL)));
    MD_CHECK(nostr_event_sign(wrap, dm_ephemeral_sk) == 0);
    if (fixture == DM_WRAP_SIG) wrap->sig[0] = wrap->sig[0] == 'a' ? 'b' : 'a';
    if (fixture == DM_WRAP_ID) wrap->id[0] = wrap->id[0] == 'a' ? 'b' : 'a';
    free(pk); free(sender_pk); free(ephemeral_pk);
    md_signer_destroy(sender); md_signer_destroy(ephemeral);
    return wrap;
}

typedef struct {
    MdSigner *direct;
    MdNostr *bridge;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool block, entered, released, draining, hold_admission, admitted;
    int delivered, rejected;
    unsigned signer_waiting;
    MdNostrDmResult last_error;
    atomic_int decrypts, signed_events, serializations, destroyed;
} DmState;
static int dm_test_pk(MdSigner *s, char **out) {
    return md_signer_get_pubkey(((DmState *)s->backend)->direct, out);
}
static int dm_test_decrypt(MdSigner *s, const char *peer, const char *ct, char **out) {
    DmState *st = s->backend;
    atomic_fetch_add(&st->decrypts, 1);
    pthread_mutex_lock(&st->mu);
    if (st->block) {
        st->entered = true; pthread_cond_broadcast(&st->cv);
        while (!st->released) pthread_cond_wait(&st->cv, &st->mu);
    }
    pthread_mutex_unlock(&st->mu);
    return md_signer_nip44_decrypt(st->direct, peer, ct, out);
}
static int dm_test_encrypt(MdSigner *s, const char *peer, const char *text, char **out) {
    DmState *st = s->backend;
    pthread_mutex_lock(&st->mu);
    st->entered = true; pthread_cond_broadcast(&st->cv);
    while (st->block && !st->released) pthread_cond_wait(&st->cv, &st->mu);
    pthread_mutex_unlock(&st->mu);
    return md_signer_nip44_encrypt(st->direct, peer, text, out);
}
static int dm_test_sign(MdSigner *s, const char *json, char **out) {
    DmState *st = s->backend;
    atomic_fetch_add(&st->signed_events, 1);
    return md_signer_sign_event(st->direct, json, out);
}
static void dm_test_destroy(MdSigner *s) {
    atomic_fetch_add(&((DmState *)s->backend)->destroyed, 1);
}
static const MdSignerOps dm_test_ops = {
    .get_pubkey = dm_test_pk, .nip44_decrypt = dm_test_decrypt,
    .nip44_encrypt = dm_test_encrypt, .sign_event = dm_test_sign,
    .destroy = dm_test_destroy,
};
static void dm_received(const char *pk, const char *content, void *ud) {
    DmState *st = ud;
    char *expected = md_nostr_get_pubkey(dm_sender_sk);
    MD_CHECK(strcmp(pk, expected) == 0); free(expected);
    MD_CHECK(strcmp(content, "session-control") == 0);
    MD_CHECK(atomic_load(&st->destroyed) == 0);
    /* A consumer callback can still safely access bridge-owned state. */
    MD_CHECK(md_nostr_allowlist_count(st->bridge) == 0);
    st->delivered++;
}
static void dm_rejected(MdNostrDmResult result, void *ud) {
    DmState *st = ud;
    MD_CHECK(atomic_load(&st->destroyed) == 0);
    st->last_error = result; st->rejected++;
}
static void dm_test_hook(MdNostrTestPoint point, void *ud) {
    DmState *st = ud;
    pthread_mutex_lock(&st->mu);
    if (point == MD_NOSTR_TEST_SERIALIZING) atomic_fetch_add(&st->serializations, 1);
    if (point == MD_NOSTR_TEST_DRAINING) st->draining = true;
    if (point == MD_NOSTR_TEST_ADMITTED && st->hold_admission) {
        st->admitted = true; pthread_cond_broadcast(&st->cv);
        while (!st->released) pthread_cond_wait(&st->cv, &st->mu);
    }
    pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mu);
}

static MdNostr *dm_bridge_owned(DmState *st, MdAccounts *accounts) {
    memset(st, 0, sizeof(*st));
    pthread_mutex_init(&st->mu, NULL); pthread_cond_init(&st->cv, NULL);
    atomic_init(&st->decrypts, 0); atomic_init(&st->destroyed, 0);
    atomic_init(&st->signed_events, 0); atomic_init(&st->serializations, 0);
    st->direct = md_signer_create_direct(dm_recipient_sk);
    MdSigner *s = md_signer_create_custom(MD_SIGNER_NIP46, &dm_test_ops, st);
    const char *relays[] = { "ws://127.0.0.1:1" };
    MdAccountBinding *owner = NULL;
    if (accounts) {
        char *pk = NULL;
        MD_CHECK(md_signer_get_pubkey(s, &pk) == 0);
        MD_CHECK(md_accounts_add(accounts, MD_SIGNER_NIP46, pk, "shared fixture") == 0);
        MD_CHECK(md_accounts_bind_role(accounts, MD_ACCOUNT_HOST, MD_SIGNER_NIP46, pk, s) == 0);
        MD_CHECK(md_accounts_bind_role(accounts, MD_ACCOUNT_CLIENT, MD_SIGNER_NIP46, pk, NULL) == 0);
        owner = md_accounts_acquire_role(accounts, MD_ACCOUNT_HOST);
        free(pk);
    }
    MdNostrConfig cfg = { .signer = s, .owner = owner, .relay_urls = relays, .relay_count = 1 };
    MdNostrCallbacks cbs = { .on_dm = dm_received, .on_dm_error = dm_rejected,
                             .dm_userdata = st };
    st->bridge = md_nostr_create(&cfg, &cbs);
    md_account_binding_release(owner);
    MD_CHECK(st->bridge != NULL);
    md_nostr_set_test_hook(st->bridge, dm_test_hook, st);
    md_signer_destroy(s); /* bridge must retain its own reference */
    return st->bridge;
}
static MdNostr *dm_bridge(DmState *st) { return dm_bridge_owned(st, NULL); }
static void dm_state_fini(DmState *st) {
    MD_CHECK(atomic_load(&st->destroyed) == 1);
    md_signer_destroy(st->direct);
    pthread_cond_destroy(&st->cv); pthread_mutex_destroy(&st->mu);
}
static void test_dm_validation_and_dedupe(void) {
    DmState st;
    MdNostr *n = dm_bridge(&st);
    const MdNostrDmResult errors[] = {
        MD_NOSTR_DM_OK, MD_NOSTR_DM_INVALID_WRAP, MD_NOSTR_DM_INVALID_WRAP,
        MD_NOSTR_DM_WRONG_RECIPIENT, MD_NOSTR_DM_INVALID_SEAL,
        MD_NOSTR_DM_INVALID_SEAL, MD_NOSTR_DM_INVALID_SEAL,
        MD_NOSTR_DM_INVALID_RUMOR, MD_NOSTR_DM_INVALID_RUMOR,
        MD_NOSTR_DM_INVALID_RUMOR, MD_NOSTR_DM_SENDER_MISMATCH,
        MD_NOSTR_DM_WRONG_RECIPIENT, MD_NOSTR_DM_INVALID_SEAL,
        MD_NOSTR_DM_INVALID_RUMOR,
    };
    for (int fixture = DM_WRAP_SIG; fixture <= DM_RUMOR_JSON; fixture++) {
        NostrEvent *wrap = dm_fixture((DmFixture)fixture, "session-control");
        int before = atomic_load(&st.decrypts);
        int rejected = st.rejected;
        MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_OK);
        nostr_event_free(wrap); md_nostr_drain_dm(n);
        MD_CHECK(st.delivered == 0);
        MD_CHECK(st.last_error == errors[fixture]);
        MD_CHECK(st.rejected == rejected + 1);
        if (fixture <= DM_WRAP_RECIPIENT) MD_CHECK(atomic_load(&st.decrypts) == before);
    }
    NostrEvent *wrap = dm_fixture(DM_GOOD, "session-control");
    MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_OK);
    md_nostr_drain_dm(n); MD_CHECK(st.delivered == 1);
    int before = atomic_load(&st.decrypts);
    MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_OK);
    md_nostr_drain_dm(n); MD_CHECK(st.last_error == MD_NOSTR_DM_DUPLICATE);
    MD_CHECK(atomic_load(&st.decrypts) == before);
    nostr_event_free(wrap);
    wrap = dm_fixture(DM_GOOD, "session-control"); /* new nonce/wrap, same rumor */
    MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_OK);
    md_nostr_drain_dm(n); MD_CHECK(st.last_error == MD_NOSTR_DM_DUPLICATE);
    MD_CHECK(st.delivered == 1); nostr_event_free(wrap);
    wrap = nostr_event_new();
    char *large = malloc(MD_NOSTR_DM_MAX_BYTES + 1);
    memset(large, 'x', MD_NOSTR_DM_MAX_BYTES); large[MD_NOSTR_DM_MAX_BYTES] = 0;
    nostr_event_set_content(wrap, large); free(large);
    int serialized = atomic_load(&st.serializations);
    MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_TOO_LARGE);
    MD_CHECK(atomic_load(&st.serializations) == serialized);
    nostr_event_set_content(wrap, "small");
    large = malloc(MD_NOSTR_DM_MAX_BYTES + 1);
    memset(large, '\n', MD_NOSTR_DM_MAX_BYTES); large[MD_NOSTR_DM_MAX_BYTES] = 0;
    nostr_event_set_tags(wrap, nostr_tags_new(1, nostr_tag_new("p", large, NULL)));
    free(large);
    MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_TOO_LARGE);
    MD_CHECK(atomic_load(&st.serializations) == serialized);
    nostr_event_free(wrap);
    md_nostr_destroy(n); dm_state_fini(&st);
    printf("  PASS: named NIP-17/59 validation, randomized timestamps, wrap/rumor dedupe\n");
}
static void *destroy_dm_bridge(void *arg) { md_nostr_destroy(arg); return NULL; }
static void test_dm_bounded_teardown(void) {
    DmState st;
    MdNostr *n = dm_bridge(&st);
    st.block = true;
    NostrEvent *wrap = dm_fixture(DM_GOOD, "session-control");
    MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_OK);
    pthread_mutex_lock(&st.mu);
    while (!st.entered) pthread_cond_wait(&st.cv, &st.mu);
    pthread_mutex_unlock(&st.mu);
    for (int i = 0; i < MD_NOSTR_DM_QUEUE_CAPACITY; i++)
        MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_OK);
    MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_QUEUE_FULL);
    MdSigner *shared = md_signer_ref(md_nostr_get_signer(n));
    md_nostr_stop(n);
    MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_STOPPED);
    nostr_event_free(wrap);
    pthread_t destroyer;
    MD_CHECK(pthread_create(&destroyer, NULL, destroy_dm_bridge, n) == 0);
    pthread_mutex_lock(&st.mu);
    while (!st.draining) pthread_cond_wait(&st.cv, &st.mu);
    MD_CHECK(atomic_load(&st.destroyed) == 0);
    st.released = true;
    pthread_cond_broadcast(&st.cv); pthread_mutex_unlock(&st.mu);
    pthread_join(destroyer, NULL);
    MD_CHECK(st.delivered == 0); MD_CHECK(st.rejected == 0);
    MD_CHECK(atomic_load(&st.decrypts) == 1); /* no second decrypt or queued work */
    MD_CHECK(md_signer_check_connection(shared) == MD_SIGNER_OK);
    md_signer_destroy(shared);
    dm_state_fini(&st);
    printf("  PASS: bounded DM worker/queue, shared signer lifetime, in-flight teardown\n");
}

typedef struct { MdNostr *n; int result; } DmSend;
static void *send_dm_thread(void *arg) {
    DmSend *send = arg;
    char *pk = md_nostr_get_pubkey(dm_sender_sk);
    send->result = md_nostr_send_session_request(send->n, pk, "session-control");
    free(pk);
    return NULL;
}
static void test_outbound_dm_teardown(void) {
    DmState st;
    DmSend send = { .n = dm_bridge(&st) };
    st.block = true;
    pthread_t sender, destroyer;
    MD_CHECK(pthread_create(&sender, NULL, send_dm_thread, &send) == 0);
    pthread_mutex_lock(&st.mu);
    while (!st.entered) pthread_cond_wait(&st.cv, &st.mu);
    pthread_mutex_unlock(&st.mu);
    md_nostr_stop(send.n);
    MD_CHECK(pthread_create(&destroyer, NULL, destroy_dm_bridge, send.n) == 0);
    pthread_mutex_lock(&st.mu);
    while (!st.draining) pthread_cond_wait(&st.cv, &st.mu);
    MD_CHECK(atomic_load(&st.destroyed) == 0);
    st.released = true;
    pthread_cond_broadcast(&st.cv); pthread_mutex_unlock(&st.mu);
    pthread_join(sender, NULL); pthread_join(destroyer, NULL);
    MD_CHECK(send.result == -1);
    MD_CHECK(atomic_load(&st.signed_events) == 0);
    dm_state_fini(&st);
    printf("  PASS: outbound in-flight teardown discards result before sign/publish\n");
}

static void dm_signer_wait_hook(void *ud) {
    DmState *st = ud;
    pthread_mutex_lock(&st->mu);
    st->signer_waiting++; pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mu);
}
typedef struct { MdSigner *s; int result; } SharedSignerCall;
static void *hold_shared_signer(void *arg) {
    SharedSignerCall *call = arg;
    char *out = NULL;
    char *pk = md_nostr_get_pubkey(dm_sender_sk);
    call->result = md_signer_nip44_encrypt(call->s, pk, "other-role", &out);
    free(out); free(pk);
    return NULL;
}
static void test_dm_cancel_behind_shared_signer(void) {
#ifndef _WIN32
    char temporary[] = "/tmp/md-role-revoke-XXXXXX";
    char *created = mkdtemp(temporary);
    MD_CHECK(created != NULL);
    if (!created) return;
    char *directory = realpath(created, NULL);
    MdAccounts *accounts = NULL;
    MD_CHECK(md_accounts_open(directory, NULL, &accounts) == 0);
    if (!accounts) { rmdir(created); free(directory); return; }
    DmState st;
    MdNostr *n = dm_bridge_owned(&st, accounts);
    MdSigner *shared = md_signer_ref(md_nostr_get_signer(n));
    md_signer_set_test_hook(shared, dm_signer_wait_hook, &st);
    st.block = true;
    SharedSignerCall call = { .s = shared };
    pthread_t holder, destroyer;
    MD_CHECK(pthread_create(&holder, NULL, hold_shared_signer, &call) == 0);
    pthread_mutex_lock(&st.mu);
    while (!st.entered) pthread_cond_wait(&st.cv, &st.mu);
    pthread_mutex_unlock(&st.mu);
    NostrEvent *wrap = dm_fixture(DM_GOOD, "session-control");
    MD_CHECK(md_nostr_receive_dm(n, wrap) == MD_NOSTR_DM_OK);
    nostr_event_free(wrap);
    pthread_mutex_lock(&st.mu);
    while (st.signer_waiting < 2) pthread_cond_wait(&st.cv, &st.mu);
    pthread_mutex_unlock(&st.mu);
    /* Revoking host must cancel its bridge-local token, even while queued
     * behind the independently live client role's shared signer operation. */
    MD_CHECK(md_accounts_unbind_role(accounts, MD_ACCOUNT_HOST) == 0);
    MD_CHECK(pthread_create(&destroyer, NULL, destroy_dm_bridge, n) == 0);
    pthread_mutex_lock(&st.mu);
    while (!st.draining) pthread_cond_wait(&st.cv, &st.mu);
    st.released = true; pthread_cond_broadcast(&st.cv);
    pthread_mutex_unlock(&st.mu);
    pthread_join(holder, NULL); pthread_join(destroyer, NULL);
    MD_CHECK(call.result == MD_SIGNER_OK);
    MD_CHECK(atomic_load(&st.decrypts) == 0);
    MD_CHECK(st.delivered == 0 && st.rejected == 0);
    MD_CHECK(md_signer_check_connection(shared) == MD_SIGNER_OK);
    MdAccountBinding *client = md_accounts_acquire_role(accounts, MD_ACCOUNT_CLIENT);
    MD_CHECK(md_account_binding_is_current(client));
    char *out = NULL, *pk = md_nostr_get_pubkey(dm_sender_sk);
    MD_CHECK(md_account_binding_encrypt(client, pk, "client survives host revocation", &out) == 0);
    free(out); free(pk); md_account_binding_release(client);
    md_signer_destroy(shared); md_accounts_destroy(accounts); dm_state_fini(&st);
    char lock[4096]; snprintf(lock, sizeof(lock), "%s/.management.lock", directory);
    unlink(lock); rmdir(directory); free(directory);
    printf("  PASS: role-revoked DM waiting behind the other role never enters signer backend\n");
#else
    puts("  SKIP: account persistence unsupported on Windows");
#endif
}

typedef struct { MdNostr *n; int operation, result; } AllowlistRead;
static void *read_allowlist_thread(void *arg) {
    AllowlistRead *read = arg;
    MdAllowlistEntry entry;
    switch (read->operation) {
    case 0: read->result = md_nostr_allowlist_count(read->n); break;
    case 1: read->result = md_nostr_has_allowlist(read->n); break;
    case 2: read->result = md_nostr_is_allowed(read->n, "not-present"); break;
    case 3: read->result = md_nostr_refresh_allowlist(read->n); break;
    default: read->result = md_nostr_allowlist_get_entry(read->n, 0, &entry); break;
    }
    return NULL;
}
static void test_allowlist_read_teardown(void) {
    for (int operation = 0; operation < 5; operation++) {
        DmState st;
        AllowlistRead read = { .n = dm_bridge(&st), .operation = operation };
        st.hold_admission = true;
        pthread_t reader, destroyer;
        MD_CHECK(pthread_create(&reader, NULL, read_allowlist_thread, &read) == 0);
        pthread_mutex_lock(&st.mu);
        while (!st.admitted) pthread_cond_wait(&st.cv, &st.mu);
        pthread_mutex_unlock(&st.mu);
        MD_CHECK(pthread_create(&destroyer, NULL, destroy_dm_bridge, read.n) == 0);
        pthread_mutex_lock(&st.mu);
        while (!st.draining) pthread_cond_wait(&st.cv, &st.mu);
        MD_CHECK(atomic_load(&st.destroyed) == 0);
        st.released = true; pthread_cond_broadcast(&st.cv);
        pthread_mutex_unlock(&st.mu);
        pthread_join(reader, NULL); pthread_join(destroyer, NULL);
        MD_CHECK(read.result == (operation >= 3 ? -1 : 0));
        dm_state_fini(&st);
    }
    printf("  PASS: every in-flight allowlist reader participates in teardown drain\n");
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
    test_dm_validation_and_dedupe();
    test_dm_bounded_teardown();
    test_outbound_dm_teardown();
    test_allowlist_read_teardown();
    test_dm_cancel_behind_shared_signer();
    /* Bridge lifecycle test runs last — it starts/stops pool worker
     * threads that may leave OpenSSL PRNG in a non-reusable state. */
    test_bridge_lifecycle();
    printf("All nostr bridge tests passed.\n");
    return md_test_report();
}
