#include "accounts.h"
#include <stdlib.h>
#include <string.h>
#ifdef MD_SIGNER_ENABLE_NIP46
#include <nostr/nip46/nip46_client.h>
#endif

void md_account_startup_clear(MdAccountStartup *s) {
    if (!s) return;
    md_accounts_destroy(s->accounts);
    md_account_binding_release(s->binding);
    md_credential_store_destroy(s->store);
    memset(s, 0, sizeof(*s));
}
int md_account_startup_open_pinned(MdAccountRole role, const char *expected, MdAccountStartup *s) {
    if (!s) return MD_ACCOUNTS_INVALID;
    memset(s, 0, sizeof(*s));
    s->store = md_credential_store_default();
    int rc = md_accounts_open_snapshot(NULL, s->store, &s->accounts);
#ifdef _WIN32
    /* Saved accounts are unsupported here; preserve explicit/direct-IP use. */
    if (rc == MD_ACCOUNTS_UNSUPPORTED) rc = MD_ACCOUNTS_NOT_FOUND;
#endif
    if (expected && !rc) {
        const MdAccount *selected = md_accounts_role(s->accounts, role);
        if (!selected || strcmp(expected, selected->pubkey)) rc = MD_SIGNER_ERR_KEY_CHANGED;
    }
    if (!rc) rc = md_accounts_restore_role(s->accounts, role, &s->binding);
    if (rc == MD_ACCOUNTS_NOT_FOUND) { md_account_startup_clear(s); return 0; }
    if (rc) md_account_startup_clear(s);
    return rc;
}

int md_account_startup_open(MdAccountRole role, MdAccountStartup *s) {
    return md_account_startup_open_pinned(role, NULL, s);
}

int md_accounts_connect_signer(MdSignerType type, const char *pubkey,
    MdCredentialStore *store, MdSigner **out) {
    if (!out) return MD_ACCOUNTS_INVALID;
    *out = NULL;
    if (!pubkey || strlen(pubkey) != 64 || strspn(pubkey, "0123456789abcdef") != 64)
        return MD_ACCOUNTS_INVALID;
    MdSigner *s = NULL;
    switch (type) {
    case MD_SIGNER_NIP55L: s = md_signer_create_nip55l(); break;
    case MD_SIGNER_NIP5F: s = md_signer_create_nip5f(NULL); break;
    case MD_SIGNER_NIP46: {
#ifdef MD_SIGNER_ENABLE_NIP46
        MdNip46Credentials *c = NULL;
        int rc = md_credential_store_get(store, pubkey, &c);
        if (rc) return rc;
        NostrNip46Session *session = nostr_nip46_client_new();
        bool configured = session &&
            !nostr_nip46_client_set_secret(session, c->client_secret) &&
            !nostr_nip46_client_set_signer_pubkey(session, c->remote_pubkey) &&
            !nostr_nip46_session_set_relays(session, (const char *const *)c->relays, c->relay_count);
        if (configured) {
            nostr_nip46_client_set_timeout(session, 30000);
            configured = nostr_nip46_client_start(session) == 0;
        }
        /* Restoring a previously authorized transport key requires no new
         * pairing token or connect RPC. get_public_key below proves the owner. */
        if (configured) s = md_signer_create_nip46_from_session(session, c->remote_pubkey);
        if (!s && session) nostr_nip46_session_free(session);
        md_nip46_credentials_free(c);
#else
        (void)store;
        return MD_CREDENTIAL_UNAVAILABLE;
#endif
        break;
    }
    case MD_SIGNER_DIRECT_KEY: return MD_ACCOUNTS_NOT_READY;
    default: return MD_ACCOUNTS_INVALID;
    }
    if (!s) return MD_SIGNER_ERR_CONNECT;
    int rc = md_signer_check_connection(s);
    char *live = NULL;
    if (!rc) rc = md_signer_get_pubkey(s, &live);
    if (!rc && strcmp(pubkey, live)) rc = MD_SIGNER_ERR_KEY_CHANGED;
    free(live);
    if (rc) { md_signer_destroy(s); return rc; }
    *out = s; return 0;
}
