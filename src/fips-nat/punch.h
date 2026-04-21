/*
 * fips-nat — punch.h
 * UDP hole punch coordinator.
 *
 * Establishes bidirectional UDP connectivity between two peers that
 * are behind NATs, using their STUN-discovered reflexive addresses.
 *
 * Protocol:
 *   1. Both peers bind a UDP socket (same port used for STUN)
 *   2. Both send PROBE packets to each other's reflexive address
 *   3. On receiving a PROBE, respond with ACK
 *   4. On receiving an ACK, the punch is confirmed
 *   5. The socket fd is returned for use as FIPS underlay transport
 *
 * Probe/ACK packets are 24 bytes:
 *   [4] magic "MDPH"  [1] type  [1] version  [2] reserved  [16] session
 *
 * All coordination (exchanging reflexive addresses) happens via Nostr
 * DM signaling — this module only handles the UDP punch itself.
 */
#ifndef MD_PUNCH_H
#define MD_PUNCH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────────────────────────────────────────── */

#define MD_PUNCH_MAGIC          0x4D445048u  /* "MDPH" */
#define MD_PUNCH_VERSION        1
#define MD_PUNCH_PACKET_SIZE    24
#define MD_PUNCH_DEFAULT_TIMEOUT     10000   /* ms — total attempt time   */
#define MD_PUNCH_DEFAULT_INTERVAL    100     /* ms — between probes       */
#define MD_PUNCH_DEFAULT_BIND_PORT   0       /* ephemeral                 */

/* Punch packet types */
#define MD_PUNCH_TYPE_PROBE     0x01
#define MD_PUNCH_TYPE_ACK       0x02

/* Punch states */
typedef enum {
    MD_PUNCH_STATE_INIT,        /* not started                          */
    MD_PUNCH_STATE_PROBING,     /* sending probes, waiting for peer     */
    MD_PUNCH_STATE_GOT_PROBE,   /* received probe from peer             */
    MD_PUNCH_STATE_CONFIRMED,   /* received ACK — bidirectional path OK */
    MD_PUNCH_STATE_FAILED,      /* timeout or error                     */
} MdPunchState;

/* ── Configuration ───────────────────────────────────────────── */

typedef struct {
    char     peer_ip[64];        /* peer's reflexive IP (from STUN)     */
    uint16_t peer_port;          /* peer's reflexive port               */
    uint16_t local_bind_port;    /* local port to bind (0 = ephemeral)  */
    uint32_t timeout_ms;         /* total timeout (0 = 10s default)     */
    uint32_t probe_interval_ms;  /* time between probes (0 = 100ms)     */
    uint8_t  session_id[16];     /* shared session ID (both peers must
                                    use the same ID — exchanged via
                                    Nostr signaling)                    */
} MdPunchConfig;

/* ── Result ──────────────────────────────────────────────────── */

typedef struct {
    int      fd;                 /* connected UDP socket fd              */
    char     peer_ip[64];        /* confirmed peer address               */
    uint16_t peer_port;          /* confirmed peer port                  */
    uint16_t local_port;         /* local port that was bound            */
    uint32_t rtt_ms;             /* estimated RTT from probe/ack timing  */
} MdPunchResult;

/* ── Public API ──────────────────────────────────────────────── */

/*
 * Execute a UDP hole punch attempt (blocking).
 *
 * Sends PROBE packets to the peer's reflexive address at regular
 * intervals while listening for incoming PROBEs and ACKs. Returns
 * when bidirectional connectivity is confirmed or timeout expires.
 *
 * On success, result->fd is a connected UDP socket ready for use.
 * Caller owns the socket and must close() it when done.
 *
 * Returns 0 on success, -1 on error/timeout.
 */
int md_punch_execute(const MdPunchConfig *cfg, MdPunchResult *result);

/*
 * Simple legacy API: attempt hole punch by address string.
 * Returns the connected socket fd on success, -1 on failure.
 */
int md_punch_peer(const char *peer_addr, int peer_port);

/* ── Low-level (exposed for testing) ─────────────────────────── */

/*
 * Build a punch packet (PROBE or ACK) into buf.
 * session_id: 16-byte shared session identifier.
 * Returns MD_PUNCH_PACKET_SIZE on success, -1 on error.
 */
int md_punch_build_packet(uint8_t *buf, size_t buf_len,
                          uint8_t type, const uint8_t session_id[16]);

/*
 * Parse and validate a received punch packet.
 * Checks magic, version, and session ID match.
 * On success, sets *out_type to the packet type.
 * Returns 0 on success, -1 on invalid packet.
 */
int md_punch_parse_packet(const uint8_t *buf, size_t buf_len,
                          const uint8_t session_id[16],
                          uint8_t *out_type);

#ifdef __cplusplus
}
#endif

#endif /* MD_PUNCH_H */
