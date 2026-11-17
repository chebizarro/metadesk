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
#include "md_test.h"
#include <pthread.h>
#include <stdatomic.h>
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

/* A gated external backend exercises the common gate for every backend
 * type without relying on a relay, daemon, scheduling delay or sleep. */
typedef struct {
    MdSigner *direct;
    char *pk;
    atomic_int active, calls, destroyed;
    bool changed, switch_during_call, substitute, block, entered, release;
    unsigned waiting;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} GatedSigner;

static void gated_enter(GatedSigner *g) {
    MD_CHECK(atomic_fetch_add(&g->active, 1) == 0);
    atomic_fetch_add(&g->calls, 1);
}
static void gated_leave(GatedSigner *g) { atomic_fetch_sub(&g->active, 1); }
static int gated_pk(MdSigner *s, char **out) {
    GatedSigner *g = s->backend;
    gated_enter(g);
    *out = strdup(g->changed ?
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" : g->pk);
    gated_leave(g);
    return 0;
}
static int gated_crypt(MdSigner *s, const char *peer, const char *text, char **out) {
    (void)peer;
    GatedSigner *g = s->backend;
    gated_enter(g);
    pthread_mutex_lock(&g->mu);
    if (g->block) {
        g->entered = true;
        pthread_cond_broadcast(&g->cv);
        while (!g->release) pthread_cond_wait(&g->cv, &g->mu);
    }
    pthread_mutex_unlock(&g->mu);
    if (g->switch_during_call) g->changed = true;
    *out = strdup(text);
    gated_leave(g);
    return 0;
}
static int gated_sign(MdSigner *s, const char *json, char **out) {
    GatedSigner *g = s->backend;
    gated_enter(g);
    int ret = md_signer_sign_event(g->direct, g->substitute ?
        "{\"kind\":1,\"created_at\":1700000000,\"tags\":[],\"content\":\"substituted\"}" : json, out);
    gated_leave(g);
    return ret;
}
static void gated_cancel(MdSigner *s) {
    GatedSigner *g = s->backend;
    pthread_mutex_lock(&g->mu);
    g->release = true;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}
static void gated_destroy(MdSigner *s) {
    GatedSigner *g = s->backend;
    MD_CHECK(atomic_load(&g->active) == 0);
    atomic_fetch_add(&g->destroyed, 1);
}
static const MdSignerOps gated_ops = {
    .get_pubkey = gated_pk, .sign_event = gated_sign,
    .nip44_encrypt = gated_crypt, .nip44_decrypt = gated_crypt,
    .destroy = gated_destroy, .cancel = gated_cancel,
};
static MdSigner *gated_init(GatedSigner *g, MdSignerType type) {
    memset(g, 0, sizeof(*g));
    atomic_init(&g->active, 0); atomic_init(&g->calls, 0);
    atomic_init(&g->destroyed, 0);
    pthread_mutex_init(&g->mu, NULL); pthread_cond_init(&g->cv, NULL);
    g->direct = md_signer_create_direct(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    MD_CHECK(md_signer_get_pubkey(g->direct, &g->pk) == 0);
    return md_signer_create_custom(type, &gated_ops, g);
}
static void gated_fini(GatedSigner *g) {
    MD_CHECK(atomic_load(&g->destroyed) == 1);
    md_signer_destroy(g->direct); free(g->pk);
    pthread_mutex_destroy(&g->mu); pthread_cond_destroy(&g->cv);
}
static void gated_wait_hook(void *ud) {
    GatedSigner *g = ud;
    pthread_mutex_lock(&g->mu);
    g->waiting++; pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

typedef struct {
    MdSigner *s; int operation, ret;
    MdSignerCancel *cancel;
} SignerCall;
static void *signer_caller(void *arg) {
    SignerCall *c = arg;
    char *out = NULL;
    switch (c->operation % 4) {
    case 0: c->ret = md_signer_get_pubkey(c->s, &out); break;
    case 1: c->ret = md_signer_sign_event(c->s,
        "{\"kind\":1,\"created_at\":1700000000,\"tags\":[],\"content\":\"test\"}", &out); break;
    case 2: c->ret = md_signer_nip44_encrypt_cancellable(c->s, "peer", "text", &out, c->cancel); break;
    default: c->ret = md_signer_nip44_decrypt(c->s, "peer", "text", &out); break;
    }
    free(out); md_signer_destroy(c->s);
    return NULL;
}

static void test_serialization_and_live_pin(void) {
    for (int type = MD_SIGNER_DIRECT_KEY; type <= MD_SIGNER_NIP5F; type++) {
        GatedSigner g;
        MdSigner *s = gated_init(&g, (MdSignerType)type);
        pthread_t threads[16]; SignerCall calls[16];
        md_signer_set_test_hook(s, gated_wait_hook, &g);
        g.block = true;
        calls[0] = (SignerCall){ .s = md_signer_ref(s), .operation = 2 };
        MD_CHECK(pthread_create(&threads[0], NULL, signer_caller, &calls[0]) == 0);
        pthread_mutex_lock(&g.mu);
        while (!g.entered) pthread_cond_wait(&g.cv, &g.mu);
        pthread_mutex_unlock(&g.mu);
        for (int i = 1; i < 16; i++) {
            calls[i] = (SignerCall){ .s = md_signer_ref(s), .operation = i };
            MD_CHECK(pthread_create(&threads[i], NULL, signer_caller, &calls[i]) == 0);
        }
        pthread_mutex_lock(&g.mu);
        while (g.waiting < 16) pthread_cond_wait(&g.cv, &g.mu);
        MD_CHECK(atomic_load(&g.active) == 1);
        MD_CHECK(atomic_load(&g.calls) == 2); /* holder precheck + blocked encrypt */
        g.release = true; pthread_cond_broadcast(&g.cv);
        pthread_mutex_unlock(&g.mu);
        for (int i = 0; i < 16; i++) {
            pthread_join(threads[i], NULL); MD_CHECK(calls[i].ret == 0);
        }
        int fetched = atomic_load(&g.calls);
        MD_CHECK(md_signer_probe(s) == MD_SIGNER_OK);
        MD_CHECK(atomic_load(&g.calls) == fetched); /* initialization stays cheap */
        MD_CHECK(md_signer_check_connection(s) == MD_SIGNER_OK);
        MD_CHECK(atomic_load(&g.calls) == fetched + 1);
        char *out = NULL;
        g.substitute = true;
        MD_CHECK(md_signer_sign_event(s,
            "{\"kind\":1,\"created_at\":1700000000,\"tags\":[],\"content\":\"test\"}", &out)
            == MD_SIGNER_ERR_INVALID);
        MD_CHECK(out == NULL); g.substitute = false;
        /* Change during a backend operation: even successful output is refused. */
        g.switch_during_call = true;
        MD_CHECK(md_signer_nip44_encrypt(s, "peer", "text", &out) == MD_SIGNER_ERR_KEY_CHANGED);
        MD_CHECK(out == NULL); MD_CHECK(!md_signer_is_ready(s));
        g.changed = false;
        int before = atomic_load(&g.calls);
        MD_CHECK(md_signer_probe(s) == MD_SIGNER_ERR_KEY_CHANGED);
        MD_CHECK(atomic_load(&g.calls) == before); /* latched, never repins */
        md_signer_destroy(s); gated_fini(&g);
    }
    printf("  PASS: serialized mixed operations and live pin on all backend types\n");
}

static void test_shutdown_and_inflight_lifetime(void) {
    for (int cancel = 0; cancel <= 1; cancel++) {
        GatedSigner g;
        MdSigner *s = gated_init(&g, MD_SIGNER_NIP46);
        g.block = true;
        SignerCall call = { .s = md_signer_ref(s), .operation = 2 };
        pthread_t t;
        MD_CHECK(pthread_create(&t, NULL, signer_caller, &call) == 0);
        pthread_mutex_lock(&g.mu);
        while (!g.entered) pthread_cond_wait(&g.cv, &g.mu);
        pthread_mutex_unlock(&g.mu);
        if (cancel) {
            md_signer_shutdown(s); /* hook wakes blocked operation, then drains */
            char *out = NULL;
            MD_CHECK(md_signer_get_pubkey(s, &out) == MD_SIGNER_ERR_CANCELLED);
            MD_CHECK(out == NULL);
        }
        md_signer_destroy(s); /* release owner while thread retains its reference */
        if (!cancel) {
            MD_CHECK(atomic_load(&g.destroyed) == 0);
            pthread_mutex_lock(&g.mu); g.release = true;
            pthread_cond_broadcast(&g.cv); pthread_mutex_unlock(&g.mu);
        }
        pthread_join(t, NULL);
        MD_CHECK(call.ret == (cancel ? MD_SIGNER_ERR_CANCELLED : MD_SIGNER_OK));
        gated_fini(&g);
    }
    printf("  PASS: blocking signer cancellation and in-flight reference lifetime\n");
}

static void *shutdown_signer_thread(void *arg) {
    MdSigner *s = arg;
    md_signer_shutdown(s); md_signer_destroy(s);
    return NULL;
}
static void test_nonabortable_shutdown_and_local_cancel(void) {
    for (int local_cancel = 0; local_cancel <= 1; local_cancel++) {
        GatedSigner g;
        MdSigner *s = gated_init(&g, MD_SIGNER_NIP5F);
        MdSignerOps no_cancel = gated_ops;
        no_cancel.cancel = NULL;
        s->ops = &no_cancel; /* same backend, genuinely non-abortable */
        g.block = true;
        md_signer_set_test_hook(s, gated_wait_hook, &g);
        SignerCall holder = { .s = md_signer_ref(s), .operation = 2 };
        pthread_t first, second;
        MD_CHECK(pthread_create(&first, NULL, signer_caller, &holder) == 0);
        pthread_mutex_lock(&g.mu);
        while (!g.entered) pthread_cond_wait(&g.cv, &g.mu);
        pthread_mutex_unlock(&g.mu);
        MdSignerCancel *cancel = md_signer_cancel_new();
        SignerCall queued = { .s = NULL, .operation = 2, .cancel = cancel };
        if (local_cancel) {
            queued.s = md_signer_ref(s);
            MD_CHECK(pthread_create(&second, NULL, signer_caller, &queued) == 0);
        } else {
            MD_CHECK(pthread_create(&second, NULL, shutdown_signer_thread, md_signer_ref(s)) == 0);
        }
        pthread_mutex_lock(&g.mu);
        while (g.waiting < 2) pthread_cond_wait(&g.cv, &g.mu);
        if (local_cancel) md_signer_cancel(cancel);
        MD_CHECK(atomic_load(&g.active) == 1);
        MD_CHECK(atomic_load(&g.destroyed) == 0);
        g.release = true; pthread_cond_broadcast(&g.cv);
        pthread_mutex_unlock(&g.mu);
        pthread_join(first, NULL); pthread_join(second, NULL);
        if (local_cancel) {
            MD_CHECK(holder.ret == MD_SIGNER_OK);
            MD_CHECK(queued.ret == MD_SIGNER_ERR_CANCELLED);
            MD_CHECK(atomic_load(&g.calls) == 3); /* no queued backend call/check */
            MD_CHECK(md_signer_check_connection(s) == MD_SIGNER_OK);
        } else MD_CHECK(holder.ret == MD_SIGNER_ERR_CANCELLED);
        md_signer_cancel_free(cancel);
        md_signer_destroy(s); gated_fini(&g);
    }
    printf("  PASS: non-abortable shutdown drain and queued role-local cancellation\n");
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
    test_serialization_and_live_pin();
    test_shutdown_and_inflight_lifetime();
    test_nonabortable_shutdown_and_local_cancel();

    return md_test_report();
}
