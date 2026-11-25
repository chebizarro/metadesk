#include "model.h"
#include "identity_resolver.h"
#include <nostr/nip19/nip19.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

static gboolean account_result(int rc, GError **error) {
    if (!rc) return TRUE;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s (%d)",
                md_accounts_error_string(rc), rc);
    return FALSE;
}

char *md_manager_config_directory(void) {
    return g_build_filename(g_get_user_config_dir(), "metadesk", NULL);
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

char *md_manager_relay_normalize(const char *text, GError **error) {
    g_autoptr(GUri) uri = NULL;
    g_autofree char *lower_scheme = NULL;
    g_autofree char *lower_host = NULL;
    if (!text || !*text || strlen(text) > 2048 || strpbrk(text, " \t\r\n\\")) goto invalid;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p < 0x21 || *p == 0x7f) goto invalid;
    uri = g_uri_parse(text, G_URI_FLAGS_ENCODED_PATH, NULL);
    if (!uri) goto invalid;
    const char *scheme = g_uri_get_scheme(uri), *host = g_uri_get_host(uri);
    if (!scheme || (g_ascii_strcasecmp(scheme, "ws") && g_ascii_strcasecmp(scheme, "wss")) ||
        !host || !*host || g_uri_get_userinfo(uri) || g_uri_get_query(uri) ||
        g_uri_get_fragment(uri) || g_uri_get_port(uri) == 0) goto invalid;
    lower_scheme = g_ascii_strdown(scheme, -1);
    lower_host = g_ascii_strdown(host, -1);
    int port = g_uri_get_port(uri);
    if ((!strcmp(lower_scheme, "wss") && port == 443) ||
        (!strcmp(lower_scheme, "ws") && port == 80)) port = -1;
    const char *path = g_uri_get_path(uri);
    return g_uri_join(G_URI_FLAGS_ENCODED_PATH, lower_scheme, NULL, lower_host, port,
                      path && *path ? path : "/", NULL, NULL);
invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Use a ws:// or wss:// relay URL without credentials, query parameters, or a fragment");
    return NULL;
}

char **md_manager_relays_load(const char *directory, GError **error) {
    g_autofree char *path = g_build_filename(directory, "manager-relays.ini", NULL);
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_autoptr(GError) local = NULL;
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &local)) {
        if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) return g_new0(char *, 1);
        g_propagate_error(error, g_steal_pointer(&local)); return NULL;
    }
    char **relays = g_key_file_get_string_list(file, "relays", "bootstrap", NULL, error);
    if (!relays) return NULL;
    for (size_t i = 0; relays[i]; ++i) {
        g_autofree char *valid = md_manager_relay_normalize(relays[i], error);
        if (!valid) { g_strfreev(relays); return NULL; }
        g_free(relays[i]); relays[i] = g_steal_pointer(&valid);
    }
    return relays;
}

gboolean md_manager_relays_save(const char *directory, const char *const *relays, GError **error) {
    g_autoptr(GPtrArray) normalized = g_ptr_array_new_with_free_func(g_free);
    for (size_t i = 0; relays && relays[i]; ++i) {
        char *url = md_manager_relay_normalize(relays[i], error);
        if (!url) return FALSE;
        if (!g_ptr_array_find_with_equal_func(normalized, url, (GEqualFunc)g_str_equal, NULL))
            g_ptr_array_add(normalized, url);
        else g_free(url);
    }
    /* Reuse the account writer's directory ownership checks and writer lock. */
    MdAccounts *guard = NULL;
    if (!account_result(md_accounts_open(directory, NULL, &guard), error)) return FALSE;
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_set_string_list(file, "relays", "bootstrap",
        (const char *const *)normalized->pdata, normalized->len);
    gsize length;
    g_autofree char *data = g_key_file_to_data(file, &length, NULL);
    g_autofree char *path = g_build_filename(directory, "manager-relays.ini", NULL);
    gboolean ok = g_file_set_contents_full(path, data, length,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    md_accounts_destroy(guard);
    return ok;
}
