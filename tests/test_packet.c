/*
 * metadesk — test_packet.c
 * Wire format round-trip tests.
 */
#include "packet.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_header_round_trip(void) {
    MdPacketHeader hdr_out = {
        .version      = MD_PROTOCOL_VERSION,
        .type         = MD_PKT_VIDEO_FRAME,
        .flags        = 0,
        .payload_len  = 1024,
        .sequence     = 42,
        .timestamp_ms = 1700000000,
    };

    uint8_t buf[MD_PACKET_HEADER_SIZE];
    int ret = md_packet_header_write(&hdr_out, buf, sizeof(buf));
    assert(ret == MD_PACKET_HEADER_SIZE);

    MdPacketHeader hdr_in;
    ret = md_packet_header_read(&hdr_in, buf, sizeof(buf));
    assert(ret == 0);

    assert(hdr_in.version == hdr_out.version);
    assert(hdr_in.type == hdr_out.type);
    assert(hdr_in.flags == hdr_out.flags);
    assert(hdr_in.payload_len == hdr_out.payload_len);
    assert(hdr_in.sequence == hdr_out.sequence);
    assert(hdr_in.timestamp_ms == hdr_out.timestamp_ms);

    printf("  PASS: header round-trip\n");
}

static void test_packet_encode_decode(void) {
    const char *payload = "hello metadesk";
    uint32_t payload_len = (uint32_t)strlen(payload);

    uint8_t buf[256];
    int total = md_packet_encode(MD_PKT_ACTION, 7, 12345,
                                 (const uint8_t *)payload, payload_len,
                                 buf, sizeof(buf));
    assert(total == (int)(MD_PACKET_HEADER_SIZE + payload_len));

    MdPacketHeader hdr;
    const uint8_t *decoded_payload = NULL;
    int ret = md_packet_decode(buf, (size_t)total, &hdr, &decoded_payload);
    assert(ret == 0);
    assert(hdr.type == MD_PKT_ACTION);
    assert(hdr.sequence == 7);
    assert(hdr.timestamp_ms == 12345);
    assert(hdr.payload_len == payload_len);
    assert(memcmp(decoded_payload, payload, payload_len) == 0);

    printf("  PASS: packet encode/decode\n");
}

static void test_bad_version(void) {
    uint8_t buf[MD_PACKET_HEADER_SIZE] = {0};
    buf[0] = 99; /* bad version */

    MdPacketHeader hdr;
    int ret = md_packet_header_read(&hdr, buf, sizeof(buf));
    assert(ret == -1);

    printf("  PASS: bad version rejected\n");
}

static void test_buffer_too_small(void) {
    MdPacketHeader hdr = { .version = MD_PROTOCOL_VERSION };
    uint8_t buf[4]; /* too small */
    int ret = md_packet_header_write(&hdr, buf, sizeof(buf));
    assert(ret == -1);

    printf("  PASS: buffer too small rejected\n");
}

int main(void) {
    printf("test_packet:\n");
    test_header_round_trip();
    test_packet_encode_decode();
    test_bad_version();
    test_buffer_too_small();
    printf("All packet tests passed.\n");
    return 0;
}
