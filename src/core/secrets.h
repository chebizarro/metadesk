/*
 * metadesk — secrets.h
 * 1Password Connect integration for secret retrieval.
 * See spec §7 — no secrets on disk or in env vars.
 */
#ifndef MD_SECRETS_H
#define MD_SECRETS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque secrets context */
typedef struct MdSecrets MdSecrets;

/*
 * Create secrets context. connect_url is the 1Password Connect endpoint.
 * token is the bootstrap token (typically from a secure prompt or initial config).
 */
MdSecrets *md_secrets_create(const char *connect_url, const char *token);

/*
 * Retrieve a secret by item reference (e.g. "op://metadesk/fips-node/nsec").
 * Writes into buf (which should be mlock'd by caller). Returns bytes written, or -1.
 */
int md_secrets_get(MdSecrets *s, const char *item_ref,
                   uint8_t *buf, size_t buf_len);

/* Destroy secrets context, zeroing internal buffers. */
void md_secrets_destroy(MdSecrets *s);

#ifdef __cplusplus
}
#endif

#endif /* MD_SECRETS_H */
