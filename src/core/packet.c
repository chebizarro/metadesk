/*
 * metadesk — packet.c
 * Wire format encode/decode. See spec §3.1.
 */
#include "packet.h"
#include <string.h>

/* Little-endian helpers */
static void write_u16(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_u32(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF);
    buf[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t read_u16(const uint8_t *buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t read_u32(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

int md_packet_header_write(const MdPacketHeader *hdr, uint8_t *buf, size_t buf_len) {
    if (!hdr || !buf || buf_len < MD_PACKET_HEADER_SIZE)
        return -1;

    buf[0] = hdr->version;
    buf[1] = hdr->type;
    write_u16(buf + 2, hdr->flags);
    write_u32(buf + 4, hdr->payload_len);
    write_u32(buf + 8, hdr->sequence);
    write_u32(buf + 12, hdr->timestamp_ms);

    return MD_PACKET_HEADER_SIZE;
}

int md_packet_header_read(MdPacketHeader *hdr, const uint8_t *buf, size_t buf_len) {
    if (!hdr || !buf || buf_len < MD_PACKET_HEADER_SIZE)
        return -1;

    hdr->version      = buf[0];
    hdr->type         = buf[1];
    hdr->flags        = read_u16(buf + 2);
    hdr->payload_len  = read_u32(buf + 4);
    hdr->sequence     = read_u32(buf + 8);
    hdr->timestamp_ms = read_u32(buf + 12);

    if (hdr->version != MD_PROTOCOL_VERSION)
        return -1;

    return 0;
}

int md_packet_encode(uint8_t type, uint32_t seq, uint32_t ts_ms,
                     const uint8_t *payload, uint32_t payload_len,
                     uint8_t *buf, size_t buf_len) {
    /* Guard against integer overflow: cast to size_t before addition */
    size_t total = (size_t)MD_PACKET_HEADER_SIZE + (size_t)payload_len;
    if (buf_len < total)
        return -1;

    MdPacketHeader hdr = {
        .version      = MD_PROTOCOL_VERSION,
        .type         = type,
        .flags        = 0,
        .payload_len  = payload_len,
        .sequence     = seq,
        .timestamp_ms = ts_ms,
    };

    int ret = md_packet_header_write(&hdr, buf, buf_len);
    if (ret < 0)
        return -1;

    if (payload && payload_len > 0)
        memcpy(buf + MD_PACKET_HEADER_SIZE, payload, payload_len);

    return (int)(MD_PACKET_HEADER_SIZE + payload_len);
}

int md_packet_decode(const uint8_t *buf, size_t buf_len,
                     MdPacketHeader *hdr, const uint8_t **payload) {
    if (md_packet_header_read(hdr, buf, buf_len) < 0)
        return -1;

    /* Guard against integer overflow: cast to size_t before addition */
    if (buf_len < (size_t)MD_PACKET_HEADER_SIZE + (size_t)hdr->payload_len)
        return -1;

    if (payload)
        *payload = buf + MD_PACKET_HEADER_SIZE;

    return 0;
}
