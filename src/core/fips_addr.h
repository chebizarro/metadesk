/*
 * metadesk — fips_addr.h
 * FIPS address derivation and DNS resolution.
 *
 * Derives fd00::/8 ULA IPv6 addresses from Nostr npub identities.
 * The algorithm mirrors fips/src/identity/:
 *   1. Decode npub (bech32) → 32-byte x-only secp256k1 pubkey
 *   2. SHA-256(pubkey) → take first 16 bytes → node_addr
 *   3. FipsAddress = 0xfd || node_addr[0..15] → 128-bit IPv6
 *
 * Applications connect to these fd00::/8 addresses via standard
 * BSD sockets. The FIPS TUN interface (fips0) handles routing,
 * Noise IK/XK encryption, and header compression transparently.
 *
 * DNS: resolving "npub1xxx.fips" primes the FIPS identity cache,
 * enabling subsequent TUN packet routing.
 *
 * MTU: FIPS_IPV6_OVERHEAD = 77 bytes (per fips-ipv6-adapter.md).
 * Effective payload MTU = transport_mtu - 77.
 */
#ifndef MD_FIPS_ADDR_H
#define MD_FIPS_ADDR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FIPS ULA prefix byte */
#define MD_FIPS_PREFIX        0xfd

/* FIPS overhead per packet (Noise headers + IPv6 shim) */
#define MD_FIPS_IPV6_OVERHEAD 77

/* DNS suffix for FIPS names */
#define MD_FIPS_DNS_SUFFIX    ".fips"

/* Default FIPS transport MTU (typical UDP = 1280, minus overhead) */
#define MD_FIPS_EFFECTIVE_MTU (1280 - MD_FIPS_IPV6_OVERHEAD)

/* Maximum IPv6 address string length (including null terminator) */
#define MD_FIPS_IPV6_STRLEN   46

/* ── Address derivation ──────────────────────────────────────── */

/*
 * Derive a FIPS fd00::/8 IPv6 address string from an npub.
 *
 * npub: bech32-encoded Nostr public key (e.g. "npub1abc...")
 * ipv6_out: buffer for the IPv6 string (at least MD_FIPS_IPV6_STRLEN bytes)
 * ipv6_len: size of ipv6_out buffer
 *
 * Returns 0 on success, -1 on invalid npub or buffer too small.
 */
int md_fips_addr_from_npub(const char *npub,
                           char *ipv6_out, size_t ipv6_len);

/*
 * Derive a FIPS fd00::/8 IPv6 address string from a hex pubkey.
 *
 * pk_hex: 64-character hex-encoded x-only public key
 * ipv6_out: buffer for the IPv6 string
 * ipv6_len: size of ipv6_out buffer
 *
 * Returns 0 on success, -1 on error.
 */
int md_fips_addr_from_pubkey_hex(const char *pk_hex,
                                 char *ipv6_out, size_t ipv6_len);

/*
 * Derive FIPS address from raw 32-byte x-only public key.
 * Writes 16 bytes into addr_out.
 * Returns 0 on success, -1 on error.
 */
int md_fips_addr_from_pubkey(const uint8_t *pubkey, size_t pubkey_len,
                             uint8_t *addr_out);

/*
 * Format a 16-byte FIPS address as an IPv6 string.
 * Returns 0 on success, -1 if buffer too small.
 */
int md_fips_addr_to_string(const uint8_t *addr,
                           char *out, size_t out_len);

/* ── DNS resolution ──────────────────────────────────────────── */

/*
 * Build a FIPS DNS name from an npub: "npub1xxx.fips"
 * dns_out: buffer for the DNS name
 * dns_len: size of dns_out buffer
 * Returns 0 on success, -1 on error.
 */
int md_fips_dns_name(const char *npub, char *dns_out, size_t dns_len);

/*
 * Resolve a FIPS DNS name to prime the identity cache.
 *
 * Performs a getaddrinfo() lookup for "npub1xxx.fips" which the
 * FIPS DNS responder resolves to the fd00::/8 address. This primes
 * the FIPS identity cache so subsequent TUN traffic can be routed.
 *
 * ipv6_out: buffer for the resolved IPv6 address string
 * ipv6_len: size of ipv6_out buffer
 *
 * Returns 0 on success, -1 on resolution failure.
 *
 * Note: Falls back to direct computation if DNS is unavailable.
 */
int md_fips_resolve(const char *npub,
                    char *ipv6_out, size_t ipv6_len);

/* ── Validation ──────────────────────────────────────────────── */

/* Check if an IPv6 address string is in the FIPS fd00::/8 range. */
bool md_fips_is_fips_addr(const char *ipv6_str);

/* Check if a string looks like an npub (starts with "npub1"). */
bool md_fips_is_npub(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* MD_FIPS_ADDR_H */
