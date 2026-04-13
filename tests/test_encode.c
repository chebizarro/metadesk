/*
 * metadesk — tests/test_encode.c
 * Encode/decode round-trip unit tests.
 *
 * Tests the encoding pipeline (NVENC or x264) and decoding pipeline
 * by encoding synthetic frames and verifying decoded output.
 */
#include "encode.h"
#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int g_encoded_count;
static int g_decoded_count;
static uint8_t *g_last_encoded_data;
static size_t   g_last_encoded_size;

/* ── Helpers ─────────────────────────────────────────────────── */

#define TEST_W 640
#define TEST_H 480

static void fill_solid(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                       uint8_t r, uint8_t g, uint8_t b) {
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = buf + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            row[x * 4 + 0] = b;  /* B */
            row[x * 4 + 1] = g;  /* G */
            row[x * 4 + 2] = r;  /* R */
            row[x * 4 + 3] = 255;
        }
    }
}

static void on_encode(const MdEncodedPacket *pkt, void *userdata) {
    (void)userdata;
    g_encoded_count++;

    /* Save last packet for decode test */
    free(g_last_encoded_data);
    g_last_encoded_data = malloc(pkt->size);
    if (g_last_encoded_data) {
        memcpy(g_last_encoded_data, pkt->data, pkt->size);
        g_last_encoded_size = pkt->size;
    }
}

static void on_decode(const MdDecodedFrame *frame, void *userdata) {
    (void)userdata;
    g_decoded_count++;
    /* Verify frame has valid dimensions */
    assert(frame->width == TEST_W);
    assert(frame->height == TEST_H);
    assert(frame->stride == TEST_W * 4);
    assert(frame->data != NULL);
}

/* ── Test: encoder creation ──────────────────────────────────── */

static int test_encoder_create(void) {
    printf("  test_encoder_create... ");

    /* NULL config should fail */
    assert(md_encoder_create(NULL) == NULL);

    /* Zero dimensions should fail */
    MdEncoderConfig cfg = { .width = 0, .height = 480 };
    assert(md_encoder_create(&cfg) == NULL);

    /* Odd dimensions should fail (NV12 requires even) */
    cfg.width = 641; cfg.height = 480;
    assert(md_encoder_create(&cfg) == NULL);

    /* Valid config should succeed */
    cfg.width = TEST_W;
    cfg.height = TEST_H;
    cfg.bitrate = MD_ENCODER_DEFAULT_BITRATE;
    cfg.fps = 30;
    cfg.prefer_nvenc = true;

    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    uint32_t w, h;
    assert(md_encoder_get_size(enc, &w, &h) == 0);
    assert(w == TEST_W && h == TEST_H);

    printf("OK (%s)\n", md_encoder_is_hw(enc) ? "NVENC" : "x264");
    md_encoder_destroy(enc);
    return 0;
}

/* ── Test: encode a few frames ───────────────────────────────── */

static int test_encode_frames(void) {
    printf("  test_encode_frames... ");

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 4000000,
        .fps = 30,
        .prefer_nvenc = true,
    };

    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    assert(buf != NULL);

    g_encoded_count = 0;

    /* Encode 10 frames */
    for (int i = 0; i < 10; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride,
                   (uint8_t)(i * 25), 128, 64);
        int ret = md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                    (int64_t)i, on_encode, NULL);
        assert(ret == 0);
    }

    /* Flush remaining */
    md_encoder_flush(enc, on_encode, NULL);

    /* Should have gotten some encoded packets */
    assert(g_encoded_count > 0);
    printf("OK (%d packets from 10 frames)\n", g_encoded_count);

    free(buf);
    md_encoder_destroy(enc);
    return 0;
}

/* ── Test: full round-trip (encode → decode) ─────────────────── */

static int test_roundtrip(void) {
    printf("  test_roundtrip... ");

    MdEncoderConfig enc_cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 4000000,
        .fps = 30,
        .prefer_nvenc = true,
    };

    MdEncoder *enc = md_encoder_create(&enc_cfg);
    assert(enc != NULL);

    MdDecoder *dec = md_decoder_create();
    assert(dec != NULL);

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    assert(buf != NULL);

    g_encoded_count = 0;
    g_decoded_count = 0;

    /* Encode 30 frames, feeding each encoded packet to decoder */
    for (int i = 0; i < 30; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride,
                   (uint8_t)((i * 8) % 256), (uint8_t)((i * 4) % 256), 100);

        g_last_encoded_data = NULL;
        g_last_encoded_size = 0;

        int ret = md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                    (int64_t)i, on_encode, NULL);
        assert(ret == 0);

        /* If we got an encoded packet, submit to decoder */
        if (g_last_encoded_data && g_last_encoded_size > 0) {
            ret = md_decoder_submit(dec, g_last_encoded_data,
                                    g_last_encoded_size, (int64_t)i);
            assert(ret == 0);

            md_decoder_poll(dec, on_decode, NULL);
        }
    }

    /* Flush encoder and decoder */
    md_encoder_flush(enc, on_encode, NULL);
    if (g_last_encoded_data && g_last_encoded_size > 0) {
        md_decoder_submit(dec, g_last_encoded_data, g_last_encoded_size, 0);
        md_decoder_poll(dec, on_decode, NULL);
    }
    md_decoder_flush(dec, on_decode, NULL);

    printf("OK (encoded=%d, decoded=%d)\n", g_encoded_count, g_decoded_count);

    /* Should have decoded at least some frames */
    assert(g_decoded_count > 0);

    free(g_last_encoded_data);
    g_last_encoded_data = NULL;

    free(buf);
    md_decoder_destroy(dec);
    md_encoder_destroy(enc);
    return 0;
}

/* ── Test: decoder creation ──────────────────────────────────── */

static int test_decoder_create(void) {
    printf("  test_decoder_create... ");

    MdDecoder *dec = md_decoder_create();
    assert(dec != NULL);

    /* Submit garbage data — should not crash, just return error */
    uint8_t garbage[16] = {0};
    int ret = md_decoder_submit(dec, garbage, sizeof(garbage), 0);
    /* Decoder may accept or reject garbage depending on codec state — just don't crash */
    (void)ret;

    /* Poll should return 0 frames (no valid input) */
    g_decoded_count = 0;
    md_decoder_poll(dec, on_decode, NULL);
    assert(g_decoded_count == 0);

    printf("OK\n");
    md_decoder_destroy(dec);
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_encode: encode/decode round-trip tests\n");

    int failures = 0;
    failures += test_encoder_create();
    failures += test_encode_frames();
    failures += test_decoder_create();
    failures += test_roundtrip();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures;
}
