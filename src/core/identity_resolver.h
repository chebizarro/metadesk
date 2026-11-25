#ifndef MD_IDENTITY_RESOLVER_H
#define MD_IDENTITY_RESOLVER_H

#include <stdbool.h>
#include <stddef.h>

#define MD_RELAY_TARGET_MAX 16
#define MD_RELAY_URL_MAX 2048

typedef enum { MD_HINT_NONE, MD_HINT_NPROFILE, MD_HINT_NIP05 } MdIdentityHint;
typedef struct {
    char pubkey[65];
    MdIdentityHint hint_source;
    char *hints[MD_RELAY_TARGET_MAX];
    size_t hint_count;
} MdIdentity;

typedef enum {
    MD_RESOLVE_OK, MD_RESOLVE_INVALID, MD_RESOLVE_FETCH_FAILED,
    MD_RESOLVE_UNSAFE, MD_RESOLVE_NOT_FOUND, MD_RESOLVE_PIN_MISMATCH
} MdResolveResult;

/* pin, if provided, is an npub or 64-hex key. Clear out after success. */
MdResolveResult md_identity_resolve(const char *input, const char *pin, MdIdentity *out);
/* Check an already-resolved candidate against a saved pin without resolving a second time. */
MdResolveResult md_identity_check_pin(const MdIdentity *, const char *pin);
/* Normalize with the same NIP-05 parser used for lookup. Malloc-owned. */
char *md_identity_normalize_nip05(const char *input);
MdResolveResult md_identity_from_nip05_json(const char *input, const char *json,
                                            const char *pin, MdIdentity *out);
/* Returned canonical npub is malloc-owned. */
char *md_identity_npub(const MdIdentity *id);
/* Display only: the first 16 hex digits in four groups. Validate the full
 * key first; never use the short fingerprint as the actual pin. */
#define MD_KEY_FINGERPRINT_SIZE 20
bool md_key_fingerprint(const char *pubkey, char out[MD_KEY_FINGERPRINT_SIZE]);
void md_identity_clear(MdIdentity *id);
void md_identity_scrub_environment(void);
const char *md_resolve_error(MdResolveResult result);

#endif
