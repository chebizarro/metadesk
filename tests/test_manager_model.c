#include "model.h"
#include "md_test.h"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *key_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *key_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static int get_key(MdSigner *s, char **out) { *out = strdup(s->backend); return *out ? 0 : MD_SIGNER_ERR_INTERNAL; }
static void destroy_signer(MdSigner *s) { free(s->backend); }
static const MdSignerOps ops = {.get_pubkey = get_key, .destroy = destroy_signer};
static MdSigner *fake_signer(const char *key) {
    return md_signer_create_custom(MD_SIGNER_NIP5F, &ops, strdup(key));
}
static void cleanup(const char *directory) {
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            g_autofree char *path = g_build_filename(directory, name, NULL);
            MD_CHECK(g_remove(path) == 0);
        }
        g_dir_close(dir);
    }
    MD_CHECK(g_rmdir(directory) == 0);
}
static void test_public_identity(void) {
    char hex[65], fingerprint[20];
    g_autofree char *npub = md_manager_npub(key_a);
    MD_CHECK(npub && g_str_has_prefix(npub, "npub1"));
    MD_CHECK(md_manager_public_key(npub, hex, NULL));
    MD_CHECK(!strcmp(hex, key_a));
    MD_CHECK(md_manager_public_key(key_b, hex, NULL));
    MD_CHECK(!strcmp(hex, key_b));
    MD_CHECK(md_manager_fingerprint(key_a, fingerprint));
    MD_CHECK(!strcmp(fingerprint, "aaaaaaaa…aaaaaaaa"));
    MD_CHECK(md_manager_fingerprint("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", fingerprint));
    MD_CHECK(!strcmp(fingerprint, "01234567…89abcdef"));
    MD_CHECK(!md_manager_fingerprint("short", fingerprint) && !*fingerprint);
    MD_CHECK(!md_manager_npub("invalid"));
    const char *invalid[] = {NULL, "", "nsec1secret", "npub1invalid", "AAAA", "nostr:npub1invalid", NULL};
    for (size_t i = 0; i < G_N_ELEMENTS(invalid) - 1; ++i) {
        g_autoptr(GError) error = NULL;
        MD_CHECK(!md_manager_public_key(invalid[i], hex, &error));
        MD_CHECK(error != NULL);
    }
}
static void test_accounts(const char *dir) {
    g_autoptr(GError) error = NULL;
    GPtrArray *list = md_manager_accounts_load(dir, &error);
    MD_CHECK(list && list->len == 0 && !error);
    if (!list) return;
    g_ptr_array_unref(list);
    MD_CHECK(md_manager_account_add(dir, MD_SIGNER_NIP5F, key_a, "Desktop", &error));
    g_autofree char *npub = md_manager_npub(key_b);
    MD_CHECK(md_manager_account_add(dir, MD_SIGNER_NIP5F, npub, "Laptop", &error));
    MD_CHECK(!md_manager_account_add(dir, MD_SIGNER_NIP5F, key_a, "Duplicate", &error));
    g_clear_error(&error);
    MD_CHECK(!md_manager_account_add(dir, MD_SIGNER_DIRECT_KEY, key_a, "Not a vault", &error));
    g_clear_error(&error);
    MdSigner *signer = fake_signer(key_a), *wrong = fake_signer(key_b);
    MD_CHECK(signer && wrong);
    MD_CHECK(md_manager_role_save(dir, MD_ACCOUNT_CLIENT, MD_SIGNER_NIP5F, key_a, signer, &error));
    MD_CHECK(md_manager_role_save(dir, MD_ACCOUNT_HOST, MD_SIGNER_NIP5F, key_a, signer, &error));
    MD_CHECK(!md_manager_role_save(dir, MD_ACCOUNT_CLIENT, MD_SIGNER_NIP5F, key_a, wrong, &error));
    g_clear_error(&error);
    MD_CHECK(!md_manager_role_save(dir, MD_ACCOUNT_CLIENT, MD_SIGNER_NIP5F, key_b, NULL, &error));
    g_clear_error(&error);
    MD_CHECK(!md_manager_role_save(dir, MD_ACCOUNT_ROLE_COUNT, MD_SIGNER_NIP5F, NULL, NULL, &error));
    g_clear_error(&error);
    list = md_manager_accounts_load(dir, &error);
    MD_CHECK(list && list->len == 2);
    if (list && list->len == 2) {
        MdManagerAccount *first = g_ptr_array_index(list, 0);
        MD_CHECK(!strcmp(first->account.label, "Desktop"));
        MD_CHECK(first->roles[MD_ACCOUNT_CLIENT] && first->roles[MD_ACCOUNT_HOST]);
        MD_CHECK(first->npub && !strcmp(first->fingerprint, "aaaaaaaa…aaaaaaaa"));
        MdManagerAccount *second = g_ptr_array_index(list, 1);
        MD_CHECK(!second->roles[MD_ACCOUNT_CLIENT] && !second->roles[MD_ACCOUNT_HOST]);
    }
    if (list) g_ptr_array_unref(list);
    MD_CHECK(md_manager_role_save(dir, MD_ACCOUNT_CLIENT, MD_SIGNER_NIP5F, NULL, NULL, &error));
    list = md_manager_accounts_load(dir, &error);
    if (list && list->len) {
        MdManagerAccount *first = g_ptr_array_index(list, 0);
        MD_CHECK(!first->roles[MD_ACCOUNT_CLIENT] && first->roles[MD_ACCOUNT_HOST]);
        g_ptr_array_unref(list);
    } else MD_CHECK(FALSE);
    MdAccounts *writer = NULL;
    MD_CHECK(md_accounts_open(dir, NULL, &writer) == 0);
    MD_CHECK(!md_manager_account_add(dir, MD_SIGNER_NIP55L, key_a, "Busy", &error));
    g_clear_error(&error); md_accounts_destroy(writer);
    md_signer_destroy(signer); md_signer_destroy(wrong);
    MD_CHECK(!error);
}
static void test_relays(const char *dir) {
    const char *valid[][2] = {
        {"wss://Relay.Example:443", "wss://relay.example/"},
        {"ws://127.0.0.1:80", "ws://127.0.0.1/"},
        {"ws://[::1]:8080/relay", "ws://[::1]:8080/relay"},
        {"wss://relay.example/path", "wss://relay.example/path"},
        {"wss://relay.example/a%2Fb", "wss://relay.example/a%2Fb"},
        {"wss://relay.example/a%3Fb%23c", "wss://relay.example/a%3Fb%23c"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(valid); ++i) {
        g_autofree char *url = md_manager_relay_normalize(valid[i][0], NULL);
        MD_CHECK(url && !strcmp(url, valid[i][1]));
    }
    const char *invalid[] = {"", "https://relay.example", "wss://", "wss://a b/", "wss://user:secret@relay.example/",
        "wss://relay.example/?token=secret", "wss://relay.example/#secret", "ws://localhost:0", "ws://localhost:99999",
        "wss://relay.example/\\path", "wss://relay.example/\n", "not a URL", "wss://relay.example/%zz"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        g_autoptr(GError) error = NULL;
        g_autofree char *url = md_manager_relay_normalize(invalid[i], &error);
        MD_CHECK(!url && error);
    }
    g_autoptr(GError) error = NULL;
    char **relays = md_manager_relays_load(dir, &error);
    MD_CHECK(relays && !relays[0] && !error); g_strfreev(relays);
    const char *selected[] = {"wss://Relay.Example:443", "wss://relay.example/", "ws://localhost:8080", NULL};
    MD_CHECK(md_manager_relays_save(dir, selected, &error));
    relays = md_manager_relays_load(dir, &error);
    MD_CHECK(relays && g_strv_length(relays) == 2);
    if (relays) MD_CHECK(!strcmp(relays[0], "wss://relay.example/"));
    g_strfreev(relays);
    const char *bad[] = {"nonsense", NULL};
    MD_CHECK(!md_manager_relays_save(dir, bad, &error)); g_clear_error(&error);
    relays = md_manager_relays_load(dir, &error);
    MD_CHECK(relays && g_strv_length(relays) == 2); g_strfreev(relays);
    g_autofree char *path = g_build_filename(dir, "manager-relays.ini", NULL);
    struct stat st;
    MD_CHECK(!g_stat(path, &st) && (st.st_mode & 0777) == 0600);
    MD_CHECK(md_manager_relays_save(dir, NULL, &error));
    relays = md_manager_relays_load(dir, &error);
    MD_CHECK(relays && !relays[0]); g_strfreev(relays);
    MD_CHECK(g_file_set_contents(path, "[relays]\nbootstrap=https://bad.example;\n", -1, &error));
    relays = md_manager_relays_load(dir, &error);
    MD_CHECK(!relays && error); g_strfreev(relays); g_clear_error(&error);
}
int main(void) {
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("md-w11-model-XXXXXX", &error);
    MD_CHECK(dir && !error);
    if (!dir) return md_test_report();
    test_public_identity(); test_accounts(dir); test_relays(dir); cleanup(dir);
    return md_test_report();
}
