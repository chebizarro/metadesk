/*
 * fips-nat — stun.h
 * STUN address discovery (RFC 5389 Binding).
 *
 * Performs a STUN Binding Request to discover the node's reflexive
 * (public) transport address. Uses raw RFC 5389 protocol over UDP —
 * no libnice dependency for basic discovery.
 *
 * Default STUN server: stun.l.google.com:19302
 */
#ifndef MD_STUN_H
#define MD_STUN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────���───────────────────────────────────── */

#define MD_STUN_DEFAULT_SERVER  "stun.l.google.com"
#define MD_STUN_DEFAULT_PORT    19302
#define MD_STUN_DEFAULT_TIMEOUT 3000   /* ms */
#define MD_STUN_MAX_RETRIES     3

/* ── Result structure ────────────────────────────────────────── */

typedef struct {
    char     ip[64];       /* reflexive IP string (NUL-terminated)   */
    uint16_t port;         /* reflexive port (host byte order)       */
    bool     is_ipv6;      /* true if address is IPv6                */
} MdStunResult;

/* ── Public API ──────────────────────────────────────────────── */

/*
 * Discover public address via STUN Binding Request.
 *
 * stun_server: hostname or IP of STUN server (NULL → default)
 * stun_port:   STUN server port (0 → 19302)
 * result:      output struct with reflexive address + port
 * timeout_ms:  per-attempt timeout (0 → 3000ms)
 *
 * Retries up to MD_STUN_MAX_RETRIES times on timeout.
 * Returns 0 on success, -1 on error.
 */
int md_stun_discover_full(const char *stun_server, uint16_t stun_port,
                          MdStunResult *result, uint32_t timeout_ms);

/*
 * Simple wrapper: discover and write IP string to buf.
 * stun_server: "host:port" or "host" (NULL → default)
 * Returns 0 on success, -1 on error.
 */
int md_stun_discover(const char *stun_server, char *buf, int buf_len);

/* ── Low-level (exposed for testing) ─────────────────────────── */

/* STUN message types */
#define MD_STUN_BINDING_REQUEST    0x0001
#define MD_STUN_BINDING_RESPONSE   0x0101
#define MD_STUN_BINDING_ERROR      0x0111
#define MD_STUN_MAGIC_COOKIE       0x2112A442u
#define MD_STUN_HEADER_SIZE        20

/* STUN attribute types */
#define MD_STUN_ATTR_MAPPED_ADDR       0x0001
#define MD_STUN_ATTR_XOR_MAPPED_ADDR   0x0020
#define MD_STUN_ATTR_ERROR_CODE        0x0009

/* Address families */
#define MD_STUN_FAMILY_IPV4  0x01
#define MD_STUN_FAMILY_IPV6  0x02

/*
 * Build a STUN Binding Request into buf.
 * txn_id: 12-byte transaction ID (caller provides randomness).
 * Returns number of bytes written (always 20), or -1 on error.
 */
int md_stun_build_request(uint8_t *buf, size_t buf_len,
                          const uint8_t txn_id[12]);

/*
 * Parse a STUN Binding Response.
 * txn_id: expected 12-byte transaction ID (must match request).
 * Returns 0 on success and fills result, -1 on error.
 */
int md_stun_parse_response(const uint8_t *buf, size_t buf_len,
                           const uint8_t txn_id[12],
                           MdStunResult *result);

#ifdef __cplusplus
}
#endif

#endif /* MD_STUN_H */
