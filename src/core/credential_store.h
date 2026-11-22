/* NIP-46 transport credentials. Never serialize these into management.json. */
#ifndef MD_CREDENTIAL_STORE_H
#define MD_CREDENTIAL_STORE_H
#include <stddef.h>

typedef enum {
    MD_CREDENTIAL_OK = 0,
    MD_CREDENTIAL_NOT_FOUND = -200,
    MD_CREDENTIAL_LOCKED = -201,
    MD_CREDENTIAL_UNAVAILABLE = -202,
    MD_CREDENTIAL_INVALID = -203,
    MD_CREDENTIAL_FAILED = -204,
    MD_CREDENTIAL_AMBIGUOUS = -205, /* duplicate OS items: remove before re-import */
} MdCredentialError;

typedef struct MdCredentialStore MdCredentialStore;
/* Backend get returns a malloc'd NUL-terminated payload, owned by the caller.
 * remove is idempotent. No implementation may fall back to plaintext files.
 * Stores are synchronous, control-thread objects; they may block on OS IPC. */
typedef struct {
    int (*get)(void *context, const char *pubkey, char **payload);
    int (*put)(void *context, const char *pubkey, const char *payload);
    int (*remove)(void *context, const char *pubkey);
    void (*destroy)(void *context);
} MdCredentialStoreOps;
MdCredentialStore *md_credential_store_custom(const MdCredentialStoreOps *, void *context);
MdCredentialStore *md_credential_store_memory(void);
MdCredentialStore *md_credential_store_default(void);
MdCredentialStore *md_credential_store_secret_service(void);
/* NULL path selects the default Keychain. Explicit path supports isolated tests. */
MdCredentialStore *md_credential_store_keychain(const char *path);
/* Current-user DPAPI ciphertext; NULL uses LOCALAPPDATA/metadesk/credentials. */
MdCredentialStore *md_credential_store_windows(const char *directory);
void md_credential_store_destroy(MdCredentialStore *);

typedef struct {
    char user_pubkey[65];
    char remote_pubkey[65];
    char client_secret[65]; /* transport key, NOT the user's signing key */
    size_t relay_count;
    char **relays;
} MdNip46Credentials;
/* Versioned JSON format: version=1, user_pubkey, remote_pubkey, client_secret,
 * relays. Lowercase hex keys; 1..16 ws(s) URLs without embedded credentials.
 * Returned structures/payloads must be released with the wiping helpers. */
int md_nip46_credentials_parse(const char *json, MdNip46Credentials **out);
int md_nip46_credentials_encode(const MdNip46Credentials *, char **out);
void md_nip46_credentials_free(MdNip46Credentials *);
int md_credential_store_get(MdCredentialStore *, const char *pubkey, MdNip46Credentials **out);
int md_credential_store_put(MdCredentialStore *, const MdNip46Credentials *);
int md_credential_store_remove(MdCredentialStore *, const char *pubkey);
void md_credential_wipe(void *data, size_t size);
void md_credential_free(char *payload);
const char *md_credential_error_string(int error);
#endif
