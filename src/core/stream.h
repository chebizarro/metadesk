/*
 * metadesk — stream.h
 * TCP stream transport with MdPacketHeader framing.
 *
 * Provides server (listen/accept) and client (connect) TCP sockets
 * with packet-level send/receive using the wire format from packet.h.
 *
 * Design:
 *   - Blocking I/O with per-packet timeouts (simplest for Phase 1)
 *   - The server accepts a single client (1:1 session model)
 *   - All data is framed with 16-byte MdPacketHeader + payload
 *   - Latency tracking via packet timestamps
 *
 * Port 7700 per spec §4.3. In Phase 1, TCP over localhost or LAN.
 * In Phase 2, TCP over FIPS TUN (fd00::/8 addresses).
 */
#ifndef MD_STREAM_H
#define MD_STREAM_H

#include "packet.h"
#include "fips_addr.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default port per spec §4.3 */
#define MD_STREAM_PORT 7700

/* Maximum payload size (sanity limit: 16 MB) */
#define MD_STREAM_MAX_PAYLOAD (16 * 1024 * 1024)

/* Opaque stream context (wraps a connected TCP socket) */
typedef struct MdStream MdStream;

/* Opaque server context (listening socket) */
typedef struct MdStreamServer MdStreamServer;

/* Latency statistics */
typedef struct {
    uint32_t last_rtt_ms;      /* last measured round-trip time     */
    uint32_t avg_rtt_ms;       /* exponential moving average RTT    */
    uint64_t bytes_sent;       /* total bytes sent                  */
    uint64_t bytes_recv;       /* total bytes received              */
    uint32_t packets_sent;
    uint32_t packets_recv;
} MdStreamStats;

/* ── Server API ──────────────────────────────────────────────── */

/* Create a TCP server listening on the given port (IPv6 + IPv4 dual-stack).
 * bind_addr: NULL for any address, or a specific address string.
 * Returns NULL on failure. */
MdStreamServer *md_stream_server_create(const char *bind_addr, uint16_t port);

/* Accept a single client connection (blocking).
 * timeout_ms: 0 = block forever, >0 = timeout in milliseconds.
 * Returns a connected MdStream, or NULL on timeout/error. */
MdStream *md_stream_server_accept(MdStreamServer *srv, uint32_t timeout_ms);

/* Destroy server socket. Does NOT close accepted streams. */
void md_stream_server_destroy(MdStreamServer *srv);

/* ── Client API ──────────────────────────────────────────────── */

/* Connect to a host. host can be an IP address or hostname.
 * timeout_ms: 0 = OS default, >0 = connect timeout.
 * Returns a connected MdStream, or NULL on failure. */
MdStream *md_stream_connect(const char *host, uint16_t port, uint32_t timeout_ms);

/* ── Stream I/O ──────────────────────────────────────────────── */

/* Send a framed packet (header + payload) over the stream.
 * Blocks until all bytes are written or an error occurs.
 * Returns 0 on success, -1 on error. */
int md_stream_send(MdStream *s, uint8_t type, uint32_t seq,
                   const uint8_t *payload, uint32_t payload_len);

/* Receive a framed packet from the stream.
 * Blocks until a complete packet is available, timeout, or error.
 * On success: fills hdr, allocates *payload_out (caller must free).
 * timeout_ms: 0 = block forever, >0 = timeout.
 * Returns 0 on success, -1 on error, 1 on timeout. */
int md_stream_recv(MdStream *s, MdPacketHeader *hdr,
                   uint8_t **payload_out, uint32_t timeout_ms);

/* Send a ping packet. Records send timestamp for RTT measurement. */
int md_stream_send_ping(MdStream *s);

/* Check if a received packet is a pong and update RTT stats.
 * Call after md_stream_recv() with a PONG packet. */
void md_stream_handle_pong(MdStream *s, const MdPacketHeader *hdr);

/* Get current stream statistics. */
void md_stream_get_stats(const MdStream *s, MdStreamStats *stats);

/* Get the file descriptor (for poll/select integration). */
int md_stream_get_fd(const MdStream *s);

/* Check if the stream is still connected. */
bool md_stream_is_connected(const MdStream *s);

/* Close and destroy a stream. */
void md_stream_destroy(MdStream *s);

/* ── FIPS-aware connect ───────────────────────────────────────── */

/* Connect to a host via FIPS mesh.
 * npub: Nostr public key (bech32, e.g. "npub1abc...")
 *
 * Resolution order:
 *   1. DNS lookup "npub1xxx.fips" (primes FIPS identity cache)
 *   2. Fall back to direct fd00::/8 address computation
 *   3. TCP connect to the resolved IPv6 via fips0 TUN
 *
 * Returns a connected MdStream, or NULL on failure. */
MdStream *md_stream_connect_fips(const char *npub, uint16_t port,
                                 uint32_t timeout_ms);

/* ── Timestamp utility ───────────────────────────────────────── */

/* Get current time in milliseconds (monotonic clock). */
uint32_t md_stream_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* MD_STREAM_H */
