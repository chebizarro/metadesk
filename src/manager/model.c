#include "model.h"
#include "identity_resolver.h"
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

static gboolean account_result(int rc, GError **error) {
    if (!rc) return TRUE;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s (%d)",
                md_accounts_error_string(rc), rc);
    return FALSE;
}

static gboolean valid_hex(const char *text) {
    return text && strlen(text) == 64 && strspn(text, "0123456789abcdef") == 64;
}

gboolean md_manager_public_key(const char *text, char hex[65], GError **error) {
    if (valid_hex(text)) { memcpy(hex, text, 65); return TRUE; }
    uint8_t bytes[32];
    if (text && g_str_has_prefix(text, "npub1") &&
        !nostr_nip19_decode_npub(text, bytes)) {
        for (size_t i = 0; i < 32; ++i) g_snprintf(hex + 2*i, 3, "%02x", bytes[i]);
        return TRUE;
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Enter an npub or a 64-character lowercase public key, never a secret key");
    return FALSE;
}

char *md_manager_npub(const char *hex) {
    if (!valid_hex(hex)) return NULL;
    uint8_t bytes[32];
    for (size_t i = 0; i < 32; ++i)
        bytes[i] = (uint8_t)((g_ascii_xdigit_value(hex[2*i]) << 4) |
                             g_ascii_xdigit_value(hex[2*i+1]));
    char *encoded = NULL;
    if (nostr_nip19_encode_npub(bytes, &encoded)) return NULL;
    char *result = g_strdup(encoded);
    free(encoded);
    return result;
}

gboolean md_manager_fingerprint(const char *hex, char out[20]) {
    return md_key_fingerprint(hex, out);
}

static void account_free(gpointer data) {
    MdManagerAccount *account = data;
    g_free(account->npub);
    g_free(account);
}

GPtrArray *md_manager_accounts_load(const char *directory, GError **error) {
    MdAccounts *accounts = NULL;
    if (!account_result(md_accounts_open_snapshot(directory, NULL, &accounts), error)) return NULL;
    GPtrArray *result = g_ptr_array_new_with_free_func(account_free);
    for (size_t i = 0; i < md_accounts_count(accounts); ++i) {
        MdManagerAccount *item = g_new0(MdManagerAccount, 1);
        item->account = *md_accounts_at(accounts, i);
        for (int role = 0; role < MD_ACCOUNT_ROLE_COUNT; ++role)
            item->roles[role] = md_accounts_role(accounts, role) == md_accounts_at(accounts, i);
        item->npub = md_manager_npub(item->account.pubkey);
        md_manager_fingerprint(item->account.pubkey, item->fingerprint);
        g_ptr_array_add(result, item);
    }
    md_accounts_destroy(accounts);
    return result;
}

gboolean md_manager_account_add(const char *directory, MdSignerType backend,
    const char *public_key, const char *label, GError **error) {
    char hex[65];
    if (!md_manager_public_key(public_key, hex, error)) return FALSE;
    if (backend != MD_SIGNER_NIP55L && backend != MD_SIGNER_NIP5F && backend != MD_SIGNER_NIP46)
        return account_result(MD_ACCOUNTS_INVALID, error);
    MdAccounts *accounts = NULL;
    int rc = md_accounts_open(directory, NULL, &accounts);
    if (!rc) rc = md_accounts_add(accounts, backend, hex, label);
    if (!rc) rc = md_accounts_save(accounts);
    md_accounts_destroy(accounts);
    return account_result(rc, error);
}

gboolean md_manager_role_save(const char *directory, MdAccountRole role,
    MdSignerType backend, const char *key, MdSigner *signer, GError **error) {
    if (role < MD_ACCOUNT_CLIENT || role >= MD_ACCOUNT_ROLE_COUNT ||
        (key && (!signer || backend == MD_SIGNER_DIRECT_KEY)))
        return account_result(MD_ACCOUNTS_INVALID, error);
    MdAccounts *accounts = NULL;
    int rc = md_accounts_open(directory, NULL, &accounts);
    if (!rc) rc = key ? md_accounts_bind_role(accounts, role, backend, key, signer) :
                       md_accounts_unbind_role(accounts, role);
    if (!rc) rc = md_accounts_save(accounts);
    md_accounts_destroy(accounts);
    return account_result(rc, error);
}
