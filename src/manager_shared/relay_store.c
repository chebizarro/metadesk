#include "relay_store.h"
#include "accounts.h"
#include <glib/gstdio.h>
#include <string.h>

char *md_manager_config_directory(void) {
    return g_build_filename(g_get_user_config_dir(), "metadesk", NULL);
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
    int rc = md_accounts_open(directory, NULL, &guard);
    if (rc) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s (%d)", md_accounts_error_string(rc), rc);
        return FALSE;
    }
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
