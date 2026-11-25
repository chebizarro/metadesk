/* Plain-C account metadata and independently revocable host/client roles. */
#ifndef MD_ACCOUNTS_H
#define MD_ACCOUNTS_H
#include "signer.h"
#include "credential_store.h"

typedef enum { MD_ACCOUNT_CLIENT, MD_ACCOUNT_HOST, MD_ACCOUNT_ROLE_COUNT } MdAccountRole;
typedef enum {
    MD_ACCOUNTS_OK = 0,
    MD_ACCOUNTS_INVALID = -100,
    MD_ACCOUNTS_NOT_FOUND = -101,
    MD_ACCOUNTS_EXISTS = -102,
    MD_ACCOUNTS_IO = -103,
    MD_ACCOUNTS_BUSY = -104,
    MD_ACCOUNTS_NOT_READY = -105,
    MD_ACCOUNTS_FIPS_KEY_MISMATCH = -106,
    MD_ACCOUNTS_UNSUPPORTED = -107,
} MdAccountsError;
typedef struct {
    MdSignerType backend;
    char pubkey[65];
    char label[129];
} MdAccount;
typedef struct MdAccounts MdAccounts;
typedef struct MdAccountBinding MdAccountBinding;
typedef struct MdAccountWatch MdAccountWatch;
/* Control-plane objects are single-owner, not concurrently mutable. Leases
 * below are thread-safe and may outlive the controller. A retained advisory
 * lock rejects concurrent writers to the same directory (no lost CLI updates).
 * NULL directory = XDG_CONFIG_HOME/metadesk, macOS Application Support,
 * or Windows LOCALAPPDATA/metadesk/accounts.
 * NULL store is allowed for metadata-only use, but NIP-46 removal fails closed.
 * The store is borrowed and must outlive the accounts controller.
 * Windows uses native owner-only DACLs; secret persistence uses user DPAPI.
 * Metadata getters borrow until the next mutation. */
int md_accounts_open(const char *directory, MdCredentialStore *store, MdAccounts **out);
void md_accounts_destroy(MdAccounts *);
/* Runtime snapshot: load atomically under the writer lock, then release it so
 * host and client can coexist. save is refused; CLI edits apply on restart. */
int md_accounts_open_snapshot(const char *directory, MdCredentialStore *, MdAccounts **out);
size_t md_accounts_count(const MdAccounts *);
const MdAccount *md_accounts_at(const MdAccounts *, size_t index);
int md_accounts_add(MdAccounts *, MdSignerType, const char *pubkey, const char *label);
int md_accounts_label(MdAccounts *, MdSignerType, const char *pubkey, const char *label);
/* Revokes both roles and removes only metadesk-held NIP-46 credentials.
 * Never deletes a key in an external signer. A credential deletion failure
 * leaves metadata and live roles unchanged. */
int md_accounts_remove(MdAccounts *, MdSignerType, const char *pubkey);
const MdAccount *md_accounts_role(const MdAccounts *, MdAccountRole);
/* Binding checks the live key against both signer's immutable pin and account
 * metadata. NULL signer reuses this account's cached signer. Same account in
 * two roles shares one signer but never a cancellation token. Rebinding the
 * same account revalidates it without unnecessarily revoking its generation.
 * On failure the existing role remains unchanged. Takes a signer reference. */
int md_accounts_bind_role(MdAccounts *, MdAccountRole, MdSignerType, const char *pubkey, MdSigner *);
int md_accounts_unbind_role(MdAccounts *, MdAccountRole);
/* Metadata and role selections only; explicit atomic save, mode 0600. Loaded
 * selections have no live leases until bind_role supplies a checked signer.
 * Failed saves do not undo in-memory edits; callers must report the failure. */
int md_accounts_save(MdAccounts *);
/* FIPS node pubkey from show_status, not an npub. Direct-IP callers omit this
 * check. It fails closed if host selection is missing or differs. No I/O. */
int md_accounts_check_fips_host(const MdAccounts *, const char *node_pubkey);

MdAccountBinding *md_accounts_acquire_role(MdAccounts *, MdAccountRole);
MdAccountBinding *md_account_binding_ref(MdAccountBinding *);
void md_account_binding_release(MdAccountBinding *);
bool md_account_binding_is_current(const MdAccountBinding *);
uint64_t md_account_binding_generation(const MdAccountBinding *);
MdAccountRole md_account_binding_role(const MdAccountBinding *);
/* Synchronous revocation notification. unwatch waits for a running callback.
 * Callbacks must not watch/unwatch or mutate this controller. The watch retains
 * its binding. An already-revoked binding calls back before watch returns. */
MdAccountWatch *md_account_binding_watch(MdAccountBinding *, void (*revoked)(void *), void *);
void md_account_binding_unwatch(MdAccountWatch *);
/* Borrowed signer/token valid until this binding is released. Use cancellable
 * operations and check is_current before committing asynchronous side effects.
 * Revocation discards non-abortable results; it does not kill a shared signer. */
MdSigner *md_account_binding_signer(const MdAccountBinding *);
const MdSignerCancel *md_account_binding_cancel(const MdAccountBinding *);
int md_account_binding_sign_event(MdAccountBinding *, const char *json, char **out);
int md_account_binding_encrypt(MdAccountBinding *, const char *peer, const char *text, char **out);
int md_account_binding_decrypt(MdAccountBinding *, const char *peer, const char *text, char **out);

/* CLI/helper restoration for local signer transports or saved NIP-46 pairing.
 * Direct keys must be supplied by an explicit integration via bind_role. */
int md_accounts_connect_signer(MdSignerType, const char *pubkey,
    MdCredentialStore *, MdSigner **out);
/* Restore and verify the saved selection; NOT_FOUND means no selected role.
 * Any other failure must not fall back to another identity. */
int md_accounts_restore_role(MdAccounts *, MdAccountRole, MdAccountBinding **out);
typedef struct {
    MdAccounts *accounts;
    MdCredentialStore *store;
    MdAccountBinding *binding;
} MdAccountStartup;
/* Default snapshot + OS store (no prompt or plaintext fallback). No selection
 * succeeds with binding=NULL. Explicit signer flags should bypass this. */
int md_account_startup_open(MdAccountRole, MdAccountStartup *);
/* Reject a changed role selection before contacting the signer backend. */
int md_account_startup_open_pinned(MdAccountRole, const char *expected_pubkey, MdAccountStartup *);
void md_account_startup_clear(MdAccountStartup *);
const char *md_accounts_backend_name(MdSignerType);
int md_accounts_parse_backend(const char *, MdSignerType *out);
const char *md_accounts_error_string(int error);
#endif
