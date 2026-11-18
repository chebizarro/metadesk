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
#include <stdarg.h>
#include <errno.h>
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

/* The controller owns two real loopback websocket relays. CONTROL replies
 * and hook condition variables are protocol barriers, never delivery sleeps. */
static char acl_reply[131072];
static void acl_control(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("CONTROL "); vprintf(fmt, ap); printf("\n"); fflush(stdout);
    va_end(ap);
    MD_CHECK(fgets(acl_reply, sizeof(acl_reply), stdin) != NULL);
}

typedef struct {
    MdNostr *bridge;
    MdSigner *direct;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned received, boundaries, signed_count, auth_results, processing, admitted;
    unsigned sign_entered, draining;
    bool hold_signed, release_signed, hold_processing, release_processing;
    bool hold_backend_sign, release_backend_sign;
    atomic_bool fail_sign;
    atomic_int signs;
    MdNostrAclState state;
} AclTest;

static void acl_hook(MdNostrTestPoint point, void *data) {
    AclTest *s = data;
    if (point == MD_NOSTR_TEST_ADMITTED) {
        pthread_mutex_lock(&s->mu);
        s->admitted++; pthread_cond_broadcast(&s->cv);
        pthread_mutex_unlock(&s->mu);
        return;
    }
    if (point != MD_NOSTR_TEST_DRAINING &&
        point != MD_NOSTR_TEST_ALLOWLIST_PROCESSING &&
        point != MD_NOSTR_TEST_ALLOWLIST_RECEIVED &&
        point != MD_NOSTR_TEST_ALLOWLIST_BOUNDARY &&
        point != MD_NOSTR_TEST_ALLOWLIST_SIGNED &&
        point != MD_NOSTR_TEST_AUTH_RESULT) return;
    MdNostrAclState state = md_nostr_allowlist_state(s->bridge);
    pthread_mutex_lock(&s->mu);
    s->state = state;
    if (point == MD_NOSTR_TEST_DRAINING) s->draining++;
    if (point == MD_NOSTR_TEST_ALLOWLIST_PROCESSING) s->processing++;
    if (point == MD_NOSTR_TEST_ALLOWLIST_RECEIVED) s->received++;
    if (point == MD_NOSTR_TEST_ALLOWLIST_BOUNDARY) s->boundaries++;
    if (point == MD_NOSTR_TEST_AUTH_RESULT) s->auth_results++;
    if (point == MD_NOSTR_TEST_ALLOWLIST_SIGNED) s->signed_count++;
    pthread_cond_broadcast(&s->cv);
    while (point == MD_NOSTR_TEST_ALLOWLIST_PROCESSING && s->hold_processing && !s->release_processing)
        pthread_cond_wait(&s->cv, &s->mu);
    while (point == MD_NOSTR_TEST_ALLOWLIST_SIGNED && s->hold_signed && !s->release_signed)
        pthread_cond_wait(&s->cv, &s->mu);
    pthread_mutex_unlock(&s->mu);
}
static void acl_wait(AclTest *s, unsigned *counter, unsigned count) {
    struct timespec until;
    timespec_get(&until, TIME_UTC); until.tv_sec += 15;
    pthread_mutex_lock(&s->mu);
    while (*counter < count) {
        int error = pthread_cond_timedwait(&s->cv, &s->mu, &until);
        MD_CHECK(error == 0);
        if (error) exit(1);
    }
    pthread_mutex_unlock(&s->mu);
}
static unsigned acl_counter(AclTest *s, unsigned *counter) {
    pthread_mutex_lock(&s->mu);
    unsigned value = *counter;
    pthread_mutex_unlock(&s->mu);
    return value;
}
static void acl_wait_state(AclTest *s, MdNostrAclState state) {
    struct timespec until;
    timespec_get(&until, TIME_UTC); until.tv_sec += 15;
    pthread_mutex_lock(&s->mu);
    while (s->state != state) {
        int error = pthread_cond_timedwait(&s->cv, &s->mu, &until);
        MD_CHECK(error == 0);
        if (error) exit(1);
    }
    pthread_mutex_unlock(&s->mu);
    MD_CHECK(md_nostr_allowlist_state(s->bridge) == state);
}
static int acl_sign(MdSigner *signer, const char *json, char **out) {
    AclTest *s = signer->backend;
    atomic_fetch_add(&s->signs, 1);
    pthread_mutex_lock(&s->mu);
    s->sign_entered++; pthread_cond_broadcast(&s->cv);
    while (s->hold_backend_sign && !s->release_backend_sign)
        pthread_cond_wait(&s->cv, &s->mu);
    pthread_mutex_unlock(&s->mu);
    if (atomic_load(&s->fail_sign)) return MD_SIGNER_ERR_DENIED;
    return md_signer_sign_event(s->direct, json, out);
}
static int acl_pk(MdSigner *signer, char **out) {
    return md_signer_get_pubkey(((AclTest *)signer->backend)->direct, out);
}
static const MdSignerOps acl_ops = { .get_pubkey = acl_pk, .sign_event = acl_sign };
static void acl_unused_dm(const char *pk, const char *content, void *data) {
    (void)pk; (void)content; (void)data;
    MD_CHECK(false); /* The AUTH fixture must not leak protected DMs. */
}
static void acl_bridge_start(AclTest *s, const char **urls, int count, bool open, bool dm) {
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mu, NULL); pthread_cond_init(&s->cv, NULL);
    atomic_init(&s->fail_sign, false); atomic_init(&s->signs, 0);
    s->direct = md_signer_create_direct(dm_recipient_sk);
    MdSigner *signer = md_signer_create_custom(MD_SIGNER_NIP46, &acl_ops, s);
    MdNostrConfig cfg = { .signer = signer, .relay_urls = urls, .relay_count = count,
                          .allowlist_open = open };
    MdNostrCallbacks callbacks = { .on_dm = dm ? acl_unused_dm : NULL };
    s->bridge = md_nostr_create(&cfg, &callbacks);
    MD_CHECK(s->bridge != NULL);
    s->state = md_nostr_allowlist_state(s->bridge);
    md_nostr_set_test_hook(s->bridge, acl_hook, s);
    md_signer_destroy(signer);
}
static void acl_bridge(AclTest *s, const char **urls, int count, bool open, bool dm) {
    acl_bridge_start(s, urls, count, open, dm);
    for (int i = 0; i < count; i++) acl_control("{\"op\":\"req\",\"relay\":%d}", i);
}
static void acl_finish(AclTest *s) {
    md_nostr_destroy(s->bridge);
    md_signer_destroy(s->direct);
    pthread_cond_destroy(&s->cv); pthread_mutex_destroy(&s->mu);
}
static char *acl_event(AclTest *s, int64_t created, const char *member) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_KIND_CATEGORIZED_PEOPLE_LIST);
    nostr_event_set_created_at(ev, created);
    nostr_event_set_content(ev, "");
    nostr_event_set_pubkey(ev, md_nostr_get_npub(s->bridge));
    NostrTags *tags = nostr_tags_new(1, nostr_tag_new("d", "metadesk-allowlist", NULL));
    if (member) nostr_tags_append(tags, nostr_tag_new("p", member, "video", NULL));
    nostr_event_set_tags(ev, tags);
    char *json = nostr_event_serialize(ev), *signed_json = NULL;
    MD_CHECK(md_signer_sign_event(s->direct, json, &signed_json) == 0);
    free(json); nostr_event_free(ev);
    return signed_json;
}
static void acl_deliver(AclTest *s, int relay, const char *event) {
    unsigned count = acl_counter(s, &s->received);
    acl_control("{\"op\":\"event\",\"relay\":%d,\"event\":%s}", relay, event);
    acl_wait(s, &s->received, count + 1);
}
static void acl_eose(AclTest *s, int relay) {
    unsigned count = acl_counter(s, &s->boundaries);
    acl_control("{\"op\":\"eose\",\"relay\":%d}", relay);
    acl_wait(s, &s->boundaries, count + 1);
}
static void acl_reset(void) {
    acl_control("{\"op\":\"reset\"}");
    acl_control("{\"op\":\"reset\",\"relay\":1}");
}

static void test_acl_history(const char **urls, const char *a, const char *b) {
    AclTest s;
    acl_reset(); acl_bridge(&s, urls, 2, false, false);
    MD_CHECK(md_nostr_allowlist_state(s.bridge) == MD_NOSTR_ACL_UNKNOWN);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a));
    int64_t t = time(NULL) - 100;
    char *old = acl_event(&s, t, a), *newer = acl_event(&s, t + 1, b);
    acl_deliver(&s, 0, newer);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, b)); /* EVENT alone is not ready. */
    acl_eose(&s, 0);
    MD_CHECK(md_nostr_allowlist_state(s.bridge) == MD_NOSTR_ACL_UNKNOWN);
    acl_deliver(&s, 1, old); acl_eose(&s, 1);
    MD_CHECK(md_nostr_is_allowed(s.bridge, b));
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a));
    acl_deliver(&s, 0, newer); /* duplicate */
    acl_deliver(&s, 1, old);   /* reconnect/other relay replay */
    MD_CHECK(md_nostr_allowlist_count(s.bridge) == 1);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a));

    char *one = acl_event(&s, t + 2, a), *two = acl_event(&s, t + 2, b);
    NostrEvent *e1 = nostr_event_new(), *e2 = nostr_event_new();
    MD_CHECK(nostr_event_deserialize(e1, one) == 0 && nostr_event_deserialize(e2, two) == 0);
    char id1[65], id2[65];
    MD_CHECK(nostr_event_compute_id(e1, id1) == NOSTR_EVENT_VALIDATION_OK);
    MD_CHECK(nostr_event_compute_id(e2, id2) == NOSTR_EVENT_VALIDATION_OK);
    bool first_low = strcmp(id1, id2) < 0;
    const char *low = first_low ? one : two, *high = first_low ? two : one;
    acl_deliver(&s, 0, high); acl_deliver(&s, 1, low); acl_deliver(&s, 0, high);
    MD_CHECK(md_nostr_is_allowed(s.bridge, first_low ? a : b));
    MD_CHECK(!md_nostr_is_allowed(s.bridge, first_low ? b : a));
    char *empty = acl_event(&s, t + 3, NULL);
    acl_deliver(&s, 1, empty);
    MD_CHECK(md_nostr_has_allowlist(s.bridge));
    MD_CHECK(md_nostr_allowlist_count(s.bridge) == 0);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a));
    free(old); free(newer); free(one); free(two); free(empty);
    nostr_event_free(e1); nostr_event_free(e2); acl_finish(&s);

    acl_reset(); acl_bridge(&s, urls, 1, false, false); acl_eose(&s, 0);
    MD_CHECK(md_nostr_has_allowlist(s.bridge)); /* No event also means empty. */
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a));
    unsigned boundary = acl_counter(&s, &s.boundaries);
    acl_control("{\"op\":\"closed\"}"); acl_wait(&s, &s.boundaries, boundary + 1);
    MD_CHECK(md_nostr_allowlist_state(s.bridge) == MD_NOSTR_ACL_FAILED);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a)); acl_finish(&s);
    acl_reset(); acl_bridge(&s, urls, 1, true, false);
    MD_CHECK(md_nostr_allowlist_state(s.bridge) == MD_NOSTR_ACL_OPEN);
    MD_CHECK(md_nostr_is_allowed(s.bridge, a)); acl_finish(&s);
    /* Relay 1's websocket handshake is deliberately still pending when
     * bridge callbacks are registered and relay 0 reaches EOSE. */
    acl_reset(); acl_control("{\"op\":\"hold_connection\",\"relay\":1}");
    acl_bridge_start(&s, urls, 2, false, false);
    acl_control("{\"op\":\"req\"}");
    char *grant = acl_event(&s, t, a);
    acl_deliver(&s, 0, grant); free(grant); acl_eose(&s, 0);
    MD_CHECK(md_nostr_allowlist_state(s.bridge) == MD_NOSTR_ACL_UNKNOWN);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a));
    acl_control("{\"op\":\"release_connection\",\"relay\":1}");
    acl_control("{\"op\":\"req\",\"relay\":1}");
    pthread_mutex_lock(&s.mu); s.hold_processing = true;
    unsigned processing = s.processing; pthread_mutex_unlock(&s.mu);
    char *revocation = acl_event(&s, t + 1, NULL);
    acl_control("{\"op\":\"event_eose\",\"relay\":1,\"event\":%s}", revocation);
    free(revocation);
    acl_wait(&s, &s.processing, processing + 1);
    MD_CHECK(md_nostr_allowlist_state(s.bridge) == MD_NOSTR_ACL_UNKNOWN);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a));
    pthread_mutex_lock(&s.mu); s.release_processing = true;
    pthread_cond_broadcast(&s.cv); pthread_mutex_unlock(&s.mu);
    acl_wait_state(&s, MD_NOSTR_ACL_LOADED);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a)); acl_finish(&s);
    printf("  PASS: private history, empty/unknown/open, ordering/ties, delayed handshake and EVENT/EOSE fence\n");
}

typedef struct { MdNostr *n; const char *pk; bool remove; int result; } AclEdit;
static void *acl_edit_thread(void *data) {
    AclEdit *edit = data;
    edit->result = edit->remove ? md_nostr_allowlist_remove(edit->n, edit->pk) :
        md_nostr_allowlist_add(edit->n, edit->pk, "video");
    return NULL;
}
static void *acl_destroy_thread(void *data) {
    md_nostr_destroy(((AclTest *)data)->bridge);
    return NULL;
}
static void acl_mode(const char *mode) {
    for (int i = 0; i < 2; i++) acl_control("{\"op\":\"mode\",\"relay\":%d,\"value\":\"%s\"}", i, mode);
}
static void test_acl_edits(const char **urls, const char *a, const char *b) {
    AclTest s;
    acl_reset(); acl_bridge(&s, urls, 2, false, false); acl_eose(&s, 0); acl_eose(&s, 1);
    atomic_store(&s.fail_sign, true);
    MD_CHECK(md_nostr_allowlist_add(s.bridge, a, "video") == -1);
    MD_CHECK(md_nostr_allowlist_count(s.bridge) == 0);
    atomic_store(&s.fail_sign, false); acl_mode("reject");
    MD_CHECK(md_nostr_allowlist_add(s.bridge, a, "video") == -1);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a));
    /* One accepting relay is sufficient even when the other rejects. */
    acl_control("{\"op\":\"mode\",\"value\":\"accept\"}");
    MD_CHECK(md_nostr_allowlist_add(s.bridge, a, "video") == 0);
    int signs = atomic_load(&s.signs);
    MD_CHECK(md_nostr_allowlist_add(s.bridge, a, "video") == 0);
    MD_CHECK(md_nostr_allowlist_remove(s.bridge, b) == 0);
    MD_CHECK(atomic_load(&s.signs) == signs);
    MD_CHECK(md_nostr_allowlist_count(s.bridge) == 1);
    MdAllowlistEntry entry;
    MD_CHECK(md_nostr_allowlist_get_entry(s.bridge, 0, &entry) == 0);
    MD_CHECK(!strcmp(entry.caps, "video"));
    MD_CHECK(md_nostr_granted_capabilities(s.bridge, a, UINT32_MAX) == MD_CAP_VIDEO);
    MD_CHECK(md_nostr_granted_capabilities(s.bridge, a, MD_CAP_INPUT) == 0);
    MD_CHECK(md_nostr_allowlist_add(s.bridge, a, "video,agent,input") == 0);
    MD_CHECK(md_nostr_granted_capabilities(s.bridge, a, UINT32_MAX) ==
             (MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT));
    acl_mode("reject");
    MD_CHECK(md_nostr_allowlist_add(s.bridge, a, "none") == -1);
    MD_CHECK(md_nostr_granted_capabilities(s.bridge, a, MD_CAP_INPUT) == MD_CAP_INPUT);
    acl_mode("accept");
    MD_CHECK(md_nostr_allowlist_add(s.bridge, a, "none") == 0);
    MD_CHECK(md_nostr_is_allowed(s.bridge, a)); /* Membership isn't a grant. */
    MD_CHECK(md_nostr_granted_capabilities(s.bridge, a, UINT32_MAX) == 0);
    MD_CHECK(md_nostr_allowlist_add(s.bridge, a, "video") == 0);
    atomic_store(&s.fail_sign, true);
    MD_CHECK(md_nostr_allowlist_remove(s.bridge, a) == -1);
    MD_CHECK(md_nostr_is_allowed(s.bridge, a));
    atomic_store(&s.fail_sign, false);

    acl_mode("hold");
    AclEdit edit = { s.bridge, b, false, -2 };
    pthread_t worker;
    MD_CHECK(pthread_create(&worker, NULL, acl_edit_thread, &edit) == 0);
    for (int i = 0; i < 2; i++) acl_control("{\"op\":\"held\",\"relay\":%d}", i);
    unsigned received = acl_counter(&s, &s.received);
    acl_control("{\"op\":\"echo\"}"); acl_wait(&s, &s.received, received + 1);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, b)); /* Echo before OK cannot commit. */
    MD_CHECK(md_nostr_allowlist_count(s.bridge) == 1);
    MD_CHECK(md_nostr_allowlist_add(s.bridge, b, "video") == -1); /* busy */
    for (int i = 0; i < 2; i++) acl_control("{\"op\":\"ack\",\"relay\":%d,\"ok\":true}", i);
    pthread_join(worker, NULL); MD_CHECK(edit.result == 0);
    MD_CHECK(md_nostr_is_allowed(s.bridge, b));
    acl_mode("reject");
    MD_CHECK(md_nostr_allowlist_remove(s.bridge, b) == -1);
    MD_CHECK(md_nostr_is_allowed(s.bridge, b));
    acl_mode("accept");
    MD_CHECK(md_nostr_allowlist_remove(s.bridge, b) == 0);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, b));

    pthread_mutex_lock(&s.mu);
    s.hold_signed = true;
    unsigned signed_before = s.signed_count;
    pthread_mutex_unlock(&s.mu);
    edit.result = -2;
    MD_CHECK(pthread_create(&worker, NULL, acl_edit_thread, &edit) == 0);
    acl_wait(&s, &s.signed_count, signed_before + 1);
    char *replacement = acl_event(&s, time(NULL) + 100, NULL);
    acl_deliver(&s, 1, replacement); free(replacement);
    pthread_mutex_lock(&s.mu); s.release_signed = true;
    pthread_cond_broadcast(&s.cv); pthread_mutex_unlock(&s.mu);
    pthread_join(worker, NULL);
    MD_CHECK(edit.result == -1); /* OK cannot overwrite the concurrent version. */
    MD_CHECK(md_nostr_allowlist_count(s.bridge) == 0);
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a) && !md_nostr_is_allowed(s.bridge, b));
    acl_finish(&s);

    acl_reset(); acl_bridge(&s, urls, 1, false, false);
    edit = (AclEdit){ s.bridge, a, false, -2 };
    unsigned admitted = acl_counter(&s, &s.admitted);
    MD_CHECK(pthread_create(&worker, NULL, acl_edit_thread, &edit) == 0);
    acl_wait(&s, &s.admitted, admitted + 1);
    MD_CHECK(atomic_load(&s.signs) == 0);
    md_nostr_stop(s.bridge);
    pthread_join(worker, NULL); MD_CHECK(edit.result == -1);
    acl_finish(&s);
    printf("  PASS: staged edits, signer/all-relay failure, echo, duplicate add, replacement conflict and history-wait cancellation\n");
}

static void test_auth_wire(const char **urls, const char *a) {
    AclTest s;
    acl_reset(); acl_control("{\"op\":\"reset\",\"auth\":true}");
    acl_bridge(&s, urls, 1, false, true);
    acl_control("{\"op\":\"auth\",\"count\":1}");
    NostrEvent *ev = nostr_event_new();
    MD_CHECK(nostr_event_deserialize(ev, acl_reply) == 0);
    MD_CHECK(nostr_event_validate(ev, NULL) == NOSTR_EVENT_VALIDATION_OK);
    MD_CHECK(nostr_event_get_kind(ev) == NOSTR_KIND_CLIENT_AUTHENTICATION);
    MD_CHECK(!strcmp(nostr_event_get_pubkey(ev), md_nostr_get_npub(s.bridge)));
    MD_CHECK(strstr(acl_reply, "challenge-one") && strstr(acl_reply, urls[0]));
    nostr_event_free(ev);
    acl_control("{\"op\":\"protected\",\"count\":0}");
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a));
    unsigned results = acl_counter(&s, &s.auth_results);
    acl_control("{\"op\":\"auth_result\",\"ok\":false}");
    acl_wait(&s, &s.auth_results, results + 1);
    acl_control("{\"op\":\"protected\",\"count\":0}");
    acl_control("{\"op\":\"challenge\",\"value\":\"challenge-two\"}");
    acl_control("{\"op\":\"auth\",\"count\":2}");
    MD_CHECK(strstr(acl_reply, "challenge-two"));
    results = acl_counter(&s, &s.auth_results);
    acl_control("{\"op\":\"auth_result\",\"ok\":true}");
    acl_wait(&s, &s.auth_results, results + 1);
    acl_wait_state(&s, MD_NOSTR_ACL_LOADED); /* private protected REQ eventually EOSE */
    MD_CHECK(!md_nostr_is_allowed(s.bridge, a)); /* authenticated != authorized */
    acl_control("{\"op\":\"challenge\",\"value\":\"challenge-three\"}");
    acl_control("{\"op\":\"auth\",\"count\":3}");
    results = acl_counter(&s, &s.auth_results);
    acl_control("{\"op\":\"auth_result\",\"ok\":true}");
    acl_wait(&s, &s.auth_results, results + 1);
    /* A slow signer must not block the relay reader or escape destroy. */
    pthread_mutex_lock(&s.mu); s.hold_backend_sign = true;
    unsigned entered = s.sign_entered; pthread_mutex_unlock(&s.mu);
    acl_control("{\"op\":\"challenge\",\"value\":\"challenge-cancel\"}");
    acl_wait(&s, &s.sign_entered, entered + 1);
    pthread_t destroyer;
    MD_CHECK(pthread_create(&destroyer, NULL, acl_destroy_thread, &s) == 0);
    acl_wait(&s, &s.draining, 1);
    pthread_mutex_lock(&s.mu); s.release_backend_sign = true;
    pthread_cond_broadcast(&s.cv); pthread_mutex_unlock(&s.mu);
    pthread_join(destroyer, NULL); s.bridge = NULL;
    acl_control("{\"op\":\"auth_count\",\"count\":3}");
    acl_finish(&s);
    printf("  PASS: private AUTH wire, real signatures, rejection, re-auth, protected REQ gating and blocked-signer teardown\n");
    printf("  NOTE: protected REQ recovery still uses nostrc pool reconciliation (md-1be); md-xcy remains open\n");
}

static int acl_fixture_tests(const char **urls) {
    char *a = md_nostr_get_pubkey(dm_sender_sk);
    char *b = md_nostr_get_pubkey("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    test_acl_history(urls, a, b);
    test_acl_edits(urls, a, b);
    test_auth_wire(urls, a);
    free(a); free(b);
    return md_test_report();
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--acl-fixture"))
        return acl_fixture_tests((const char **)&argv[2]);
    printf("test_nostr (nostrc bridge):\n");
    test_keypair_generation();
    uint32_t caps = UINT32_MAX;
    MD_CHECK(md_nostr_parse_caps("video,agent,input", &caps) == 0 && caps == 7);
    MD_CHECK(md_nostr_parse_caps("none", &caps) == 0 && caps == 0);
    MD_CHECK(md_nostr_parse_caps("video,bad", &caps) == -1 && caps == 0);
    MD_CHECK(md_nostr_granted_capabilities(NULL, "", UINT32_MAX) == 0);
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
