/*
 * metadesk — secrets.h
 * 1Password Connect integration for secret retrieval.
 *
 * Spec §7: No secrets on disk or in env vars. All sensitive material
 * (nsec, relay tokens, etc.) is retrieved at runtime from 1Password
 * Connect and held in mlock'd memory.
 *
 * Bootstrap: The 1PC token is the only secret the operator provides
 * (via secure prompt or process stdin). Everything else is fetched
 * from the 1Password vault via the Connect REST API.
 *
 * API: 1Password Connect Server v1
 *   - GET /v1/vaults                           → list vaults
 *   - GET /v1/vaults/{id}/items?filter=...     → search items
 *   - GET /v1/vaults/{id}/items/{id}           → get item + fields
 *
 * Item references use op:// syntax: "op://vault/item/field"
 */
#ifndef MD_SECRETS_H
#define MD_SECRETS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque secrets context */
typedef struct MdSecrets MdSecrets;

/*
 * Create secrets context.
 * connect_url: 1Password Connect server URL (e.g. "http://localhost:8080")
 * token: 1Password Connect bearer token (bootstrap secret)
 *
 * The token is copied into mlock'd memory and the original is zeroed.
 * Returns NULL on failure (invalid args, mlock failure).
 */
MdSecrets *md_secrets_create(const char *connect_url, const char *token);

/*
 * Retrieve a secret by item reference.
 *
 * item_ref: "op://vault-name/item-name/field-name"
 *   - vault-name: 1Password vault name
 *   - item-name: item title in the vault
 *   - field-name: field label (e.g. "nsec", "password", "credential")
 *
 * buf: mlock'd buffer to receive the secret value
 * buf_len: size of buf
 *
 * Returns number of bytes written to buf, or -1 on error.
 * The returned data is NOT null-terminated unless the field value is
 * shorter than buf_len, in which case a null byte is appended.
 */
int md_secrets_get(MdSecrets *s, const char *item_ref,
                   uint8_t *buf, size_t buf_len);

/*
 * Check if the secrets backend is reachable.
 * Performs a GET /v1/activity (lightweight health check).
 * Returns true if the server responds with 2xx.
 */
bool md_secrets_is_connected(MdSecrets *s);

/* Destroy secrets context, zeroing all internal buffers. */
void md_secrets_destroy(MdSecrets *s);

#ifdef __cplusplus
}
#endif

#endif /* MD_SECRETS_H */
