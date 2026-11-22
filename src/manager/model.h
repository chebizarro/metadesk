#pragma once
#include "accounts.h"
#include <gio/gio.h>

typedef struct {
    MdAccount account;
    gboolean roles[MD_ACCOUNT_ROLE_COUNT];
    char *npub;
    char fingerprint[20];
} MdManagerAccount;

char *md_manager_config_directory(void);
gboolean md_manager_public_key(const char *text, char hex[65], GError **error);
char *md_manager_npub(const char *hex);
gboolean md_manager_fingerprint(const char *hex, char out[20]);
GPtrArray *md_manager_accounts_load(const char *directory, GError **error);
gboolean md_manager_account_add(const char *directory, MdSignerType backend,
    const char *public_key, const char *label, GError **error);
/* NULL key unassigns; otherwise signer must be a live, verified backend.
 * Saves selections for process restart, not a live host reconfiguration. */
gboolean md_manager_role_save(const char *directory, MdAccountRole role,
    MdSignerType backend, const char *key, MdSigner *signer, GError **error);
char *md_manager_relay_normalize(const char *text, GError **error);
char **md_manager_relays_load(const char *directory, GError **error);
gboolean md_manager_relays_save(const char *directory, const char *const *relays,
    GError **error);
