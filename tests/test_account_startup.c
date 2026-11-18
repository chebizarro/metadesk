/* Private fixtures only: no default credential store, desktop or public relay. */
#include "accounts.h"
#include "nostr.h"
#include "md_test.h"
#include <json.h>
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#ifdef MD_SIGNER_ENABLE_NIP46
#include <nostr/nip46/nip46_bunker.h>
#include <nostr/nip46/nip46_client.h>
#endif

static const char *user_sk = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *client_sk = "0000000000000000000000000000000000000000000000000000000000000002";
static const char *other_sk = "0000000000000000000000000000000000000000000000000000000000000004";
static const char *unsigned_event = "{\"kind\":1,\"created_at\":1700000000,\"tags\":[],\"content\":\"private restore proof\"}";
static void count_revoked(void *data) { (*(unsigned *)data)++; }

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned admitted;
    bool release, allowlist_received;
} ReadGate;
typedef struct {
    MdNostr *bridge;
    const char *peer;
    int operation, result;
    MdAllowlistEntry entry;
} RoleRead;
static void hold_read(MdNostrTestPoint point, void *data) {
    if (point != MD_NOSTR_TEST_ADMITTED && point != MD_NOSTR_TEST_ALLOWLIST_RECEIVED) return;
    ReadGate *gate = data;
    pthread_mutex_lock(&gate->mutex);
    if (point == MD_NOSTR_TEST_ALLOWLIST_RECEIVED) {
        gate->allowlist_received = true;
        pthread_cond_broadcast(&gate->cond);
        pthread_mutex_unlock(&gate->mutex);
        return;
    }
    gate->admitted++;
    pthread_cond_broadcast(&gate->cond);
    while (!gate->release) pthread_cond_wait(&gate->cond, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
}
static void *read_role_cache(void *data) {
    RoleRead *read = data;
    switch (read->operation) {
    case 0: read->result = md_nostr_is_allowed(read->bridge, read->peer); break;
    case 1: read->result = md_nostr_has_allowlist(read->bridge); break;
    case 2: read->result = md_nostr_allowlist_count(read->bridge); break;
    default: read->result = md_nostr_allowlist_get_entry(read->bridge, 0, &read->entry); break;
    }
    return NULL;
}

static void test_bridge_roles(const char *directory, const char *relay) {
    MdAccounts *a = NULL;
    MD_CHECK(md_accounts_open(directory, NULL, &a) == 0);
    if (!a) return;
    char *pk = md_nostr_get_pubkey(user_sk), *other_pk = md_nostr_get_pubkey(other_sk);
    MdSigner *signer = md_signer_create_direct(user_sk);
    MdSigner *other = md_signer_create_direct(other_sk);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_DIRECT_KEY, pk, "fixture") == 0);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_DIRECT_KEY, other_pk, "replacement") == 0);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, pk, signer) == 0);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_CLIENT, MD_SIGNER_DIRECT_KEY, pk, NULL) == 0);
    MdAccountBinding *host = md_accounts_acquire_role(a, MD_ACCOUNT_HOST);
    MdAccountBinding *client = md_accounts_acquire_role(a, MD_ACCOUNT_CLIENT);
    const char *relays[] = {relay};
    MdNostrConfig cfg = {.owner = host, .relay_urls = relays, .relay_count = 1};
    unsigned host_revoked = 0, client_revoked = 0;
    MdNostrCallbacks cbs = {.on_owner_revoked = count_revoked, .owner_userdata = &host_revoked};
    MdNostr *host_bridge = md_nostr_create(&cfg, &cbs);
    cfg.owner = client; cbs.owner_userdata = &client_revoked;
    MdNostr *client_bridge = md_nostr_create(&cfg, &cbs);
    MD_CHECK(host_bridge && client_bridge);
    MD_CHECK(md_nostr_refresh_allowlist(client_bridge) == -1);
    MD_CHECK(md_nostr_allowlist_add(client_bridge, other_pk, NULL) == -1);
    MD_CHECK(md_nostr_send_session_accept(client_bridge, pk, "{}") == -1);
    MD_CHECK(md_nostr_send_session_request(host_bridge, pk, "{}") == -1);
    uint64_t generation = md_account_binding_generation(host);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, pk, NULL) == 0);
    MD_CHECK(host_revoked == 0 && md_account_binding_generation(host) == generation);
    ReadGate gate = {.release = true};
    pthread_mutex_init(&gate.mutex, NULL); pthread_cond_init(&gate.cond, NULL);
    md_nostr_set_test_hook(host_bridge, hold_read, &gate);
    MD_CHECK(md_nostr_allowlist_add(host_bridge, other_pk, "video") == 0);
    /* The private relay echoes our accepted event. Observe its consumption,
     * not just OK, before testing generation changes against the live cache. */
    pthread_mutex_lock(&gate.mutex);
    while (!gate.allowlist_received) pthread_cond_wait(&gate.cond, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);
    MD_CHECK(md_nostr_is_allowed(host_bridge, other_pk));
    MD_CHECK(md_nostr_has_allowlist(host_bridge));
    MD_CHECK(md_nostr_allowlist_count(host_bridge) == 1);
    pthread_mutex_lock(&gate.mutex); gate.admitted = 0; gate.release = false;
    pthread_mutex_unlock(&gate.mutex);
    RoleRead reads[4]; pthread_t readers[4];
    for (int i = 0; i < 4; ++i) {
        reads[i] = (RoleRead){.bridge = host_bridge, .peer = other_pk, .operation = i};
        memset(&reads[i].entry, 0x7f, sizeof(reads[i].entry));
        MD_CHECK(pthread_create(&readers[i], NULL, read_role_cache, &reads[i]) == 0);
    }
    pthread_mutex_lock(&gate.mutex);
    while (gate.admitted < 4) pthread_cond_wait(&gate.cond, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, other_pk, other) == 0);
    pthread_mutex_lock(&gate.mutex); gate.release = true;
    pthread_cond_broadcast(&gate.cond); pthread_mutex_unlock(&gate.mutex);
    for (int i = 0; i < 4; ++i) {
        pthread_join(readers[i], NULL);
        MD_CHECK(reads[i].result == (i == 3 ? -1 : 0));
    }
    MD_CHECK(!reads[3].entry.pubkey_hex[0] && !reads[3].entry.caps[0]);
    md_nostr_set_test_hook(host_bridge, NULL, NULL);
    pthread_cond_destroy(&gate.cond); pthread_mutex_destroy(&gate.mutex);
    MD_CHECK(host_revoked == 1 && client_revoked == 0);
    MD_CHECK(!md_account_binding_is_current(host) && md_account_binding_is_current(client));
    MD_CHECK(md_nostr_refresh_allowlist(host_bridge) == -1);
    MD_CHECK(!md_nostr_is_allowed(host_bridge, pk));
    MD_CHECK(md_nostr_send_session_accept(host_bridge, pk, "{}") == -1);
    char *signed_json = NULL;
    MD_CHECK(md_account_binding_sign_event(client, unsigned_event, &signed_json) == 0);
    MD_CHECK(signed_json != NULL);
    /* A replacement bridge may not publish a previous owner's cached event. */
    MdAccountBinding *replacement = md_accounts_acquire_role(a, MD_ACCOUNT_HOST);
    cfg.owner = replacement;
    MdNostr *replacement_bridge = md_nostr_create(&cfg, NULL);
    MD_CHECK(replacement_bridge != NULL);
    MD_CHECK(md_nostr_publish_signed_json(replacement_bridge, signed_json) == -1);
    md_nostr_destroy(replacement_bridge);
    md_account_binding_release(replacement);
    free(signed_json);
    /* Teardown must unregister before owner/controller destruction. */
    md_nostr_destroy(host_bridge);
    md_accounts_destroy(a);
    MD_CHECK(host_revoked == 1 && client_revoked == 1);
    MD_CHECK(md_nostr_send_session_request(client_bridge, pk, "{}") == -1);
    md_nostr_destroy(client_bridge);
    md_account_binding_release(host); md_account_binding_release(client);
    md_signer_destroy(signer); md_signer_destroy(other); free(pk); free(other_pk);
}

#ifdef MD_SIGNER_ENABLE_NIP46
/* Real nostrc server transport. Only the fixture transport client is admitted;
 * the remote transport key is deliberately distinct from the user's key. */
typedef struct { char *client_pk, *user_pk; atomic_uint requests, signatures; } Bunker;
static void bunker_request(NostrNip46BunkerRequest *request, void *data) {
    Bunker *b = data;
    const NostrNip46Request *rpc = nostr_nip46_bunker_request_rpc(request);
    if (strcmp(nostr_nip46_bunker_request_client(request), b->client_pk)) {
        nostr_nip46_bunker_request_complete(request, NULL, "wrong transport owner");
        return;
    }
    atomic_fetch_add(&b->requests, 1);
    if (!strcmp(rpc->method, "get_public_key")) {
        char quoted[68]; snprintf(quoted, sizeof(quoted), "\"%s\"", b->user_pk);
        MD_CHECK(nostr_nip46_bunker_request_complete(request, quoted, NULL) == 0);
    } else if (!strcmp(rpc->method, "sign_event") && rpc->n_params == 1) {
        NostrEvent *ev = nostr_event_new();
        char *json = NULL;
        if (ev && !nostr_event_deserialize(ev, rpc->params[0]) && !nostr_event_sign(ev, user_sk))
            json = nostr_event_serialize(ev);
        MD_CHECK(json != NULL);
        if (json) {
            atomic_fetch_add(&b->signatures, 1);
            cJSON *value = cJSON_CreateString(json);
            char *quoted = cJSON_PrintUnformatted(value);
            MD_CHECK(quoted && nostr_nip46_bunker_request_complete(request, quoted, NULL) == 0);
            free(quoted); cJSON_Delete(value);
        } else nostr_nip46_bunker_request_complete(request, NULL, "cannot sign");
        free(json); nostr_event_free(ev);
    } else nostr_nip46_bunker_request_complete(request, NULL, "unsupported fixture method");
}
static void test_real_restore(const char *directory, const char *relay) {
    unsigned char remote_sk[32] = {0}; remote_sk[31] = 3;
    Bunker b = {.client_pk = md_nostr_get_pubkey(client_sk), .user_pk = md_nostr_get_pubkey(user_sk)};
    atomic_init(&b.requests, 0); atomic_init(&b.signatures, 0);
    NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
    MD_CHECK(bunker != NULL);
    if (!bunker) return;
    MD_CHECK(nostr_nip46_bunker_set_transport_key(bunker, remote_sk) == 0);
    MD_CHECK(nostr_nip46_bunker_set_request_handler(bunker, bunker_request, &b) == 0);
    const char *relays[] = {relay};
    MD_CHECK(nostr_nip46_bunker_listen(bunker, relays, 1) == 0);
    char *remote_pk = NULL;
    MD_CHECK(nostr_nip46_bunker_get_transport_pubkey(bunker, &remote_pk) == 0);
    MdCredentialStore *store = md_credential_store_memory();
    MdNip46Credentials creds = {.relay_count = 1, .relays = (char **)relays};
    strcpy(creds.user_pubkey, b.user_pk); strcpy(creds.remote_pubkey, remote_pk);
    strcpy(creds.client_secret, client_sk);
    MD_CHECK(md_credential_store_put(store, &creds) == 0);
    MdAccounts *a = NULL;
    MD_CHECK(md_accounts_open(directory, store, &a) == 0);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_NIP46, b.user_pk, "private bunker") == 0);
    MdSigner *signer = NULL;
    MD_CHECK(md_accounts_connect_signer(MD_SIGNER_NIP46, b.user_pk, store, &signer) == 0);
    if (!signer) goto done;
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_CLIENT, MD_SIGNER_NIP46, b.user_pk, signer) == 0);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_NIP46, b.user_pk, NULL) == 0);
    MD_CHECK(md_accounts_save(a) == 0);
    md_signer_destroy(signer); signer = NULL;
    md_accounts_destroy(a); a = NULL;
    MD_CHECK(md_accounts_open_snapshot(directory, store, &a) == 0);
    MdAccountBinding *client = NULL, *host = NULL;
    MD_CHECK(md_accounts_restore_role(a, MD_ACCOUNT_CLIENT, &client) == 0 && client);
    MD_CHECK(md_accounts_restore_role(a, MD_ACCOUNT_HOST, &host) == 0 && host);
    MD_CHECK(md_account_binding_signer(client) == md_account_binding_signer(host));
    MD_CHECK(md_accounts_check_fips_host(a, b.user_pk) == 0);
    MD_CHECK(md_accounts_check_fips_host(a, remote_pk) == MD_ACCOUNTS_FIPS_KEY_MISMATCH);
    char *json = NULL;
    MD_CHECK(md_account_binding_sign_event(client, unsigned_event, &json) == 0 && json);
    NostrEvent *ev = nostr_event_new(); char canonical[65];
    MD_CHECK(json && !nostr_event_deserialize(ev, json));
    MD_CHECK(nostr_event_validate(ev, canonical) == NOSTR_EVENT_VALIDATION_OK);
    MD_CHECK(nostr_event_get_pubkey(ev) && !strcmp(nostr_event_get_pubkey(ev), b.user_pk));
    free(json); nostr_event_free(ev);
    MD_CHECK(md_accounts_unbind_role(a, MD_ACCOUNT_HOST) == 0);
    json = NULL;
    MD_CHECK(md_account_binding_sign_event(host, unsigned_event, &json) == MD_SIGNER_ERR_CANCELLED && !json);
    MD_CHECK(md_account_binding_sign_event(client, unsigned_event, &json) == 0 && json);
    free(json); md_account_binding_release(host); md_account_binding_release(client);
    md_accounts_destroy(a); a = NULL;
    /* Persist an intentionally wrong owner with valid saved transport material.
     * Restoration must ask the real bunker, not trust the credential's label. */
    char *wrong_pk = md_nostr_get_pubkey(other_sk);
    strcpy(creds.user_pubkey, wrong_pk);
    MD_CHECK(md_credential_store_put(store, &creds) == 0);
    char path[4096]; snprintf(path, sizeof(path), "%s/management.json", directory);
    FILE *f = fopen(path, "w");
    MD_CHECK(f != NULL);
    if (f) {
        fprintf(f, "{\"version\":1,\"accounts\":[{\"backend\":\"nip46\",\"pubkey\":\"%s\",\"label\":\"wrong owner\"}],\"roles\":{\"host\":null,\"client\":{\"backend\":\"nip46\",\"pubkey\":\"%s\"}}}", wrong_pk, wrong_pk);
        fclose(f);
    }
    MD_CHECK(md_accounts_open_snapshot(directory, store, &a) == 0);
    client = NULL;
    MD_CHECK(md_accounts_restore_role(a, MD_ACCOUNT_CLIENT, &client) == MD_SIGNER_ERR_KEY_CHANGED && !client);
    MD_CHECK(!md_accounts_acquire_role(a, MD_ACCOUNT_CLIENT));
    MD_CHECK(atomic_load(&b.requests) >= 3 && atomic_load(&b.signatures) == 2);
    free(wrong_pk);
done:
    md_signer_destroy(signer); md_accounts_destroy(a);
    md_credential_wipe(&creds, sizeof(creds)); md_credential_wipe(remote_sk, sizeof(remote_sk));
    md_credential_store_destroy(store);
    nostr_nip46_bunker_stop(bunker); nostr_nip46_session_free(bunker);
    free(remote_pk); free(b.client_pk); free(b.user_pk);
}
#endif

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    test_bridge_roles(argv[1], argv[3]);
#ifdef MD_SIGNER_ENABLE_NIP46
    test_real_restore(argv[2], argv[3]);
#else
    (void)client_sk;
    puts("NIP-46 real restore unavailable: build with -Dsigner_nip46=true");
#endif
    return md_test_report();
}
