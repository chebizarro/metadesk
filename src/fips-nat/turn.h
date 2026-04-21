/*
 * fips-nat — turn.h
 * TURN relay client (RFC 5766) — fallback when direct hole punch fails.
 *
 * Allocates a relay address on a TURN server (e.g. sharegap.net),
 * creates a permission + channel binding for the peer, then provides
 * a socket-like interface (send/recv via ChannelData framing).
 *
 * Wire protocol (ChannelData over TCP):
 *   [2] channel_number  [2] length  [N] data  [pad to 4-byte boundary]
 *
 * Authentication uses long-term credentials (RFC 5389 §10.2.2):
 *   username / password → HMAC-SHA1 MESSAGE-INTEGRITY.
 */
#ifndef MD_TURN_H
#define MD_TURN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────────────────────────────────────────── */

#define MD_TURN_DEFAULT_PORT          3478
#define MD_TURN_DEFAULT_TIMEOUT       5000   /* ms per request        */
#define MD_TURN_CHANNEL_MIN           0x4000 /* RFC 5766 §11          */
#define MD_TURN_CHANNEL_MAX           0x7FFE
#define MD_TURN_MAX_DATA              65535  /* max ChannelData payload */

/* STUN/TURN message types (RFC 5766 §13) */
#define MD_TURN_ALLOCATE_REQUEST      0x0003
#define MD_TURN_ALLOCATE_RESPONSE     0x0103
#define MD_TURN_ALLOCATE_ERROR        0x0113
#define MD_TURN_REFRESH_REQUEST       0x0004
#define MD_TURN_REFRESH_RESPONSE      0x0104
#define MD_TURN_CPERMISSION_REQUEST   0x0008
#define MD_TURN_CPERMISSION_RESPONSE  0x0108
#define MD_TURN_CHANBIND_REQUEST      0x0009
#define MD_TURN_CHANBIND_RESPONSE     0x0109

/* STUN attribute types used by TURN */
#define MD_TURN_ATTR_CHANNEL_NUMBER     0x000C
#define MD_TURN_ATTR_LIFETIME           0x000D
#define MD_TURN_ATTR_XOR_PEER_ADDR      0x0012
#define MD_TURN_ATTR_DATA               0x0013
#define MD_TURN_ATTR_XOR_RELAYED_ADDR   0x0016
#define MD_TURN_ATTR_REQUESTED_TRANSPORT 0x0019
#define MD_TURN_ATTR_USERNAME           0x0006
#define MD_TURN_ATTR_MESSAGE_INTEGRITY  0x0008
#define MD_TURN_ATTR_NONCE              0x0015
#define MD_TURN_ATTR_REALM              0x0014
#define MD_TURN_ATTR_ERROR_CODE         0x0009
#define MD_TURN_ATTR_SOFTWARE           0x8022

/* Transport protocol for REQUESTED-TRANSPORT */
#define MD_TURN_TRANSPORT_UDP  17

/* ── Configuration ───────────────────────────────────────────── */

typedef struct {
    char     server[256];       /* TURN server hostname or IP          */
    uint16_t port;              /* TURN server port (0 → 3478)         */
    char     username[128];     /* long-term credential username       */
    char     password[128];     /* long-term credential password       */
    char     realm[128];        /* realm (filled from 401 challenge)   */
    char     peer_ip[64];       /* peer's address to create permission */
    uint16_t peer_port;         /* peer's port                         */
    uint32_t timeout_ms;        /* per-request timeout (0 → 5000ms)    */
} MdTurnConfig;

/* ── Allocation state ────────────────────────────────────────── */

typedef struct {
    int      fd;                /* TCP socket to TURN server           */
    char     relay_ip[64];      /* relay address allocated by server   */
    uint16_t relay_port;        /* relay port allocated by server      */
    uint16_t channel;           /* channel number bound to peer        */
    uint32_t lifetime;          /* allocation lifetime in seconds      */
    char     nonce[256];        /* server nonce for auth               */
    char     realm[128];        /* server realm for auth               */
    char     username[128];     /* cached username                     */
    char     password[128];     /* cached password                     */
    bool     allocated;         /* true if allocation is active        */
    bool     channel_bound;     /* true if channel is bound to peer    */
} MdTurnAlloc;

/* ── Public API ──────────────────────────────────────────────── */

/*
 * Allocate a TURN relay and bind a channel to the peer.
 *
 * Performs the full sequence:
 *   1. TCP connect to TURN server
 *   2. Allocate (handles 401 challenge → re-send with credentials)
 *   3. CreatePermission for peer
 *   4. ChannelBind for peer
 *
 * On success, alloc->fd is ready for ChannelData send/recv.
 * Returns 0 on success, -1 on error.
 */
int md_turn_allocate(const MdTurnConfig *cfg, MdTurnAlloc *alloc);

/*
 * Send data to peer via TURN ChannelData framing.
 * Wraps payload with [channel_number][length] header.
 * Returns bytes sent (payload size) on success, -1 on error.
 */
int md_turn_send(const MdTurnAlloc *alloc, const void *data, size_t len);

/*
 * Receive data from peer via TURN ChannelData framing.
 * Strips the ChannelData header and writes payload to buf.
 * Returns bytes received on success, 0 on timeout, -1 on error.
 */
int md_turn_recv(const MdTurnAlloc *alloc, void *buf, size_t buf_len,
                 uint32_t timeout_ms);

/*
 * Refresh the TURN allocation (extend lifetime).
 * Should be called periodically (e.g. every lifetime/2 seconds).
 * Returns 0 on success, -1 on error.
 */
int md_turn_refresh(MdTurnAlloc *alloc);

/*
 * Release the TURN allocation and close the connection.
 * Sends a Refresh with lifetime=0 to the server.
 */
void md_turn_close(MdTurnAlloc *alloc);

/* ── Low-level (exposed for testing) ─────────────────────────── */

/*
 * Build a TURN Allocate Request.
 * txn_id: 12-byte transaction ID.
 * If nonce/realm/username/password are non-NULL, adds auth attributes.
 * Returns bytes written, or -1 on error.
 */
int md_turn_build_allocate(uint8_t *buf, size_t buf_len,
                           const uint8_t txn_id[12],
                           const char *username,
                           const char *realm,
                           const char *nonce,
                           const char *password);

/*
 * Build a TURN ChannelBind Request.
 * Returns bytes written, or -1 on error.
 */
int md_turn_build_channel_bind(uint8_t *buf, size_t buf_len,
                               const uint8_t txn_id[12],
                               uint16_t channel,
                               const char *peer_ip,
                               uint16_t peer_port,
                               const char *username,
                               const char *realm,
                               const char *nonce,
                               const char *password);

/*
 * Build a ChannelData frame.
 * Returns total frame size (header + data + padding), or -1 on error.
 */
int md_turn_build_channel_data(uint8_t *buf, size_t buf_len,
                               uint16_t channel,
                               const void *data, size_t data_len);

/*
 * Parse a ChannelData frame header.
 * On success, sets *out_channel and *out_length.
 * Returns 0 on success, -1 on error.
 */
int md_turn_parse_channel_data(const uint8_t *buf, size_t buf_len,
                               uint16_t *out_channel,
                               uint16_t *out_length);

/*
 * Parse a TURN Allocate Response.
 * Extracts relay address and lifetime.
 * Returns 0 on success, -1 on error.
 */
int md_turn_parse_allocate_response(const uint8_t *buf, size_t buf_len,
                                    const uint8_t txn_id[12],
                                    char *relay_ip, size_t ip_len,
                                    uint16_t *relay_port,
                                    uint32_t *lifetime);

/*
 * Parse a TURN error response.
 * Returns the error code (e.g. 401, 438), or -1 if not an error.
 */
int md_turn_parse_error(const uint8_t *buf, size_t buf_len,
                        const uint8_t txn_id[12],
                        char *nonce_out, size_t nonce_len,
                        char *realm_out, size_t realm_len);

#ifdef __cplusplus
}
#endif

#endif /* MD_TURN_H */
