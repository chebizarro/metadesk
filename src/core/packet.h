/*
 * metadesk — packet.h
 * Wire format encode/decode for frame channel packets.
 * See spec §3.1 for packet structure.
 */
#ifndef MD_PACKET_H
#define MD_PACKET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol version */
#define MD_PROTOCOL_VERSION 1

/* Packet header size in bytes (little-endian on wire) */
#define MD_PACKET_HEADER_SIZE 16

/* Packet types — spec §3.1 */
typedef enum {
    MD_PKT_VIDEO_FRAME   = 0x01,
    MD_PKT_ACTION        = 0x02,
    MD_PKT_UI_TREE       = 0x03,
    MD_PKT_UI_TREE_DELTA = 0x04,
    MD_PKT_SCREENSHOT    = 0x05,
    MD_PKT_PING          = 0x10,
    MD_PKT_PONG          = 0x11,
    MD_PKT_SESSION_INFO  = 0x20,
} MdPacketType;

/* 16-byte packet header (little-endian) */
typedef struct {
    uint8_t  version;       /* protocol version, currently 1       */
    uint8_t  type;          /* MdPacketType enum                   */
    uint16_t flags;         /* reserved, set to 0                  */
    uint32_t payload_len;   /* bytes following this header          */
    uint32_t sequence;      /* monotonic sequence number            */
    uint32_t timestamp_ms;  /* capture timestamp (ms since epoch)   */
} MdPacketHeader;

/*
 * Write header to wire buffer. buf must be >= MD_PACKET_HEADER_SIZE bytes.
 * Returns bytes written (always MD_PACKET_HEADER_SIZE) or -1 on error.
 */
int md_packet_header_write(const MdPacketHeader *hdr, uint8_t *buf, size_t buf_len);

/*
 * Read header from wire buffer.
 * Returns 0 on success, -1 on error (buffer too small, bad version).
 */
int md_packet_header_read(MdPacketHeader *hdr, const uint8_t *buf, size_t buf_len);

/*
 * Encode a complete packet (header + payload) into buf.
 * Returns total bytes written, or -1 on error.
 */
int md_packet_encode(uint8_t type, uint32_t seq, uint32_t ts_ms,
                     const uint8_t *payload, uint32_t payload_len,
                     uint8_t *buf, size_t buf_len);

/*
 * Decode a packet from buf. Fills hdr and sets *payload to point
 * into buf at the payload offset. Returns 0 on success, -1 on error.
 */
int md_packet_decode(const uint8_t *buf, size_t buf_len,
                     MdPacketHeader *hdr, const uint8_t **payload);

#ifdef __cplusplus
}
#endif

#endif /* MD_PACKET_H */
