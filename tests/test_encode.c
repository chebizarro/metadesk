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
#include <limits.h>

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

/* ── Test: reject dimensions that cannot fit FFmpeg int fields ── */

static int test_encoder_create_rejects_int_overflow(void) {
    printf("  test_encoder_create_rejects_int_overflow... ");

    MdEncoderConfig cfg = {
        .width = (uint32_t)INT_MAX + 1u,
        .height = 2,
        .bitrate = MD_ENCODER_DEFAULT_BITRATE,
        .fps = 30,
    };
    assert(md_encoder_create(&cfg) == NULL);

    cfg.width = 2;
    cfg.height = (uint32_t)INT_MAX + 1u;
    assert(md_encoder_create(&cfg) == NULL);

    cfg.height = 2;
    cfg.fps = (uint32_t)INT_MAX + 1u;
    assert(md_encoder_create(&cfg) == NULL);

    printf("OK\n");
    return 0;
}

/* ── Test: encode input stride validation ────────────────────── */

static int test_encoder_submit_rejects_short_stride(void) {
    printf("  test_encoder_submit_rejects_short_stride... ");

    MdEncoderConfig cfg = {
        .width = 64,
        .height = 64,
        .bitrate = MD_ENCODER_DEFAULT_BITRATE,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    uint8_t *buf = calloc(1, (size_t)cfg.width * cfg.height * 4u);
    assert(buf != NULL);

    assert(md_encoder_submit(enc, buf, cfg.width * 4u - 1u, MD_PIX_FMT_BGRX,
                             0, NULL, NULL) == -1);
    assert(md_encoder_submit(enc, buf, cfg.width - 1u, MD_PIX_FMT_NV12,
                             0, NULL, NULL) == -1);

    free(buf);
    md_encoder_destroy(enc);
    printf("OK\n");
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

/* ── Test: oversized decoder submissions are rejected ─────────── */

static int test_decoder_submit_oversized(void) {
    printf("  test_decoder_submit_oversized... ");

    MdDecoder *dec = md_decoder_create();
    assert(dec != NULL);

    uint8_t byte = 0;
    assert(md_decoder_submit(dec, &byte, (size_t)INT_MAX + 1u, 0) == -1);

    md_decoder_destroy(dec);
    printf("OK\n");
    return 0;
}

/* ── Test: decoder destroy NULL is safe ───────────────────────── */

static int test_decoder_destroy_null(void) {
    printf("  test_decoder_destroy_null... ");
    md_decoder_destroy(NULL); /* should not crash */
    printf("OK\n");
    return 0;
}

/* ── Test: encoder destroy NULL is safe ──────────────────────── */

static int test_encoder_destroy_null(void) {
    printf("  test_encoder_destroy_null... ");
    md_encoder_destroy(NULL); /* should not crash */
    printf("OK\n");
    return 0;
}

/* ── Test: decoder poll with no submissions ──────────────────── */

static int test_decoder_poll_empty(void) {
    printf("  test_decoder_poll_empty... ");

    MdDecoder *dec = md_decoder_create();
    assert(dec != NULL);

    g_decoded_count = 0;
    int nframes = md_decoder_poll(dec, on_decode, NULL);
    assert(nframes == 0);
    assert(g_decoded_count == 0);

    md_decoder_destroy(dec);
    printf("OK\n");
    return 0;
}

/* ── Test: decoder flush with no submissions ─────────────────── */

static int test_decoder_flush_empty(void) {
    printf("  test_decoder_flush_empty... ");

    MdDecoder *dec = md_decoder_create();
    assert(dec != NULL);

    g_decoded_count = 0;
    int nframes = md_decoder_flush(dec, on_decode, NULL);
    /* Flush with no pending data should return 0 or succeed benignly */
    assert(nframes >= 0);
    assert(g_decoded_count == 0);

    md_decoder_destroy(dec);
    printf("OK\n");
    return 0;
}

/* ── Test: encoder get_size with NULL args ────────────────────── */

static int test_encoder_get_size_null(void) {
    printf("  test_encoder_get_size_null... ");

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = MD_ENCODER_DEFAULT_BITRATE,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    /* NULL output params */
    assert(md_encoder_get_size(enc, NULL, NULL) == -1);
    uint32_t w;
    assert(md_encoder_get_size(enc, &w, NULL) == -1);
    uint32_t h;
    assert(md_encoder_get_size(enc, NULL, &h) == -1);

    /* Valid */
    assert(md_encoder_get_size(enc, &w, &h) == 0);
    assert(w == TEST_W && h == TEST_H);

    md_encoder_destroy(enc);
    printf("OK\n");
    return 0;
}

/* ── Test: encode → decode isolated frames ───────────────────── */

static int test_decode_from_encoded(void) {
    printf("  test_decode_from_encoded... ");

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 2000000,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    MdDecoder *dec = md_decoder_create();
    assert(dec != NULL);

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    assert(buf != NULL);

    g_encoded_count = 0;
    g_decoded_count = 0;

    /* Encode 5 frames, feed each to decoder */
    for (int i = 0; i < 5; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride, (uint8_t)(i * 50), 100, 50);
        free(g_last_encoded_data);
        g_last_encoded_data = NULL;
        g_last_encoded_size = 0;

        md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                          (int64_t)i, on_encode, NULL);

        if (g_last_encoded_data && g_last_encoded_size > 0) {
            int ret = md_decoder_submit(dec, g_last_encoded_data,
                                        g_last_encoded_size, (int64_t)i);
            assert(ret == 0);
            md_decoder_poll(dec, on_decode, NULL);
        }
    }

    /* Flush both */
    free(g_last_encoded_data);
    g_last_encoded_data = NULL;
    g_last_encoded_size = 0;
    md_encoder_flush(enc, on_encode, NULL);
    if (g_last_encoded_data && g_last_encoded_size > 0) {
        md_decoder_submit(dec, g_last_encoded_data, g_last_encoded_size, 0);
        md_decoder_poll(dec, on_decode, NULL);
    }
    md_decoder_flush(dec, on_decode, NULL);

    printf("OK (encoded=%d, decoded=%d)\n", g_encoded_count, g_decoded_count);

    free(g_last_encoded_data);
    g_last_encoded_data = NULL;
    free(buf);
    md_decoder_destroy(dec);
    md_encoder_destroy(enc);
    return 0;
}

/* ── Test: get_bitrate ────────────────────────────────────────── */

static int test_encoder_get_bitrate(void) {
    printf("  test_encoder_get_bitrate... ");

    /* NULL encoder should return 0 */
    assert(md_encoder_get_bitrate(NULL) == 0);

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 5000000,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    /* Should return the configured bitrate */
    assert(md_encoder_get_bitrate(enc) == 5000000);

    md_encoder_destroy(enc);

    /* Zero bitrate in config → should return default */
    cfg.bitrate = 0;
    enc = md_encoder_create(&cfg);
    assert(enc != NULL);
    assert(md_encoder_get_bitrate(enc) == MD_ENCODER_DEFAULT_BITRATE);
    md_encoder_destroy(enc);

    printf("OK\n");
    return 0;
}

/* ── Test: set_bitrate basic ─────────────────────────────────── */

static int test_encoder_set_bitrate(void) {
    printf("  test_encoder_set_bitrate... ");

    /* NULL encoder should fail */
    assert(md_encoder_set_bitrate(NULL, 4000000) == -1);

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 8000000,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    /* Set a new bitrate */
    assert(md_encoder_set_bitrate(enc, 4000000) == 0);
    assert(md_encoder_get_bitrate(enc) == 4000000);

    /* Set again */
    assert(md_encoder_set_bitrate(enc, 12000000) == 0);
    assert(md_encoder_get_bitrate(enc) == 12000000);

    md_encoder_destroy(enc);
    printf("OK\n");
    return 0;
}

/* ── Test: set_bitrate clamping ──────────────────────────────── */

static int test_encoder_set_bitrate_clamp(void) {
    printf("  test_encoder_set_bitrate_clamp... ");

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 8000000,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    /* Below minimum → clamped to MIN */
    assert(md_encoder_set_bitrate(enc, 1000) == 0);
    assert(md_encoder_get_bitrate(enc) == MD_ENCODER_MIN_BITRATE);

    /* Zero → clamped to MIN */
    assert(md_encoder_set_bitrate(enc, 0) == 0);
    assert(md_encoder_get_bitrate(enc) == MD_ENCODER_MIN_BITRATE);

    /* Above maximum → clamped to MAX */
    assert(md_encoder_set_bitrate(enc, 500000000) == 0);
    assert(md_encoder_get_bitrate(enc) == MD_ENCODER_MAX_BITRATE);

    /* Exactly at bounds */
    assert(md_encoder_set_bitrate(enc, MD_ENCODER_MIN_BITRATE) == 0);
    assert(md_encoder_get_bitrate(enc) == MD_ENCODER_MIN_BITRATE);

    assert(md_encoder_set_bitrate(enc, MD_ENCODER_MAX_BITRATE) == 0);
    assert(md_encoder_get_bitrate(enc) == MD_ENCODER_MAX_BITRATE);

    md_encoder_destroy(enc);
    printf("OK\n");
    return 0;
}

/* ── Test: encode frames after bitrate change ────────────────── */

static int test_encode_after_bitrate_change(void) {
    printf("  test_encode_after_bitrate_change... ");

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 8000000,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    assert(buf != NULL);

    g_encoded_count = 0;

    /* Encode 5 frames at 8 Mbps */
    for (int i = 0; i < 5; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride, 200, 100, 50);
        assert(md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                 (int64_t)i, on_encode, NULL) == 0);
    }
    int before = g_encoded_count;

    /* Change bitrate to 2 Mbps */
    assert(md_encoder_set_bitrate(enc, 2000000) == 0);
    assert(md_encoder_get_bitrate(enc) == 2000000);

    /* Encode 5 more frames at 2 Mbps */
    for (int i = 5; i < 10; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride, 50, 200, 100);
        assert(md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                 (int64_t)i, on_encode, NULL) == 0);
    }

    /* Change bitrate to 16 Mbps */
    assert(md_encoder_set_bitrate(enc, 16000000) == 0);
    assert(md_encoder_get_bitrate(enc) == 16000000);

    /* Encode 5 more frames at 16 Mbps */
    for (int i = 10; i < 15; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride, 100, 50, 200);
        assert(md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                 (int64_t)i, on_encode, NULL) == 0);
    }

    md_encoder_flush(enc, on_encode, NULL);

    /* Should have encoded packets across all three bitrate settings */
    assert(g_encoded_count > before);

    printf("OK (%d packets from 15 frames, 3 bitrate levels)\n", g_encoded_count);

    free(buf);
    md_encoder_destroy(enc);
    return 0;
}

/* ── Test: rapid bitrate oscillation ─────────────────────────── */

static int test_bitrate_rapid_changes(void) {
    printf("  test_bitrate_rapid_changes... ");

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 4000000,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    assert(enc != NULL);

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    assert(buf != NULL);

    g_encoded_count = 0;

    /* Change bitrate before every frame */
    uint32_t bitrates[] = {1000000, 8000000, 500000, 20000000, 3000000,
                           100000, 50000000, 2000000, 10000000, 4000000};

    for (int i = 0; i < 10; i++) {
        assert(md_encoder_set_bitrate(enc, bitrates[i]) == 0);
        fill_solid(buf, TEST_W, TEST_H, stride,
                   (uint8_t)(i * 25), (uint8_t)(255 - i * 25), 128);
        assert(md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                 (int64_t)i, on_encode, NULL) == 0);
    }

    md_encoder_flush(enc, on_encode, NULL);
    assert(g_encoded_count > 0);

    printf("OK (%d packets, 10 bitrate changes)\n", g_encoded_count);

    free(buf);
    md_encoder_destroy(enc);
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_encode: encode/decode round-trip tests\n");

    int failures = 0;
    failures += test_encoder_create();
    failures += test_encoder_create_rejects_int_overflow();
    failures += test_encoder_submit_rejects_short_stride();
    failures += test_encode_frames();
    failures += test_decoder_create();
    failures += test_roundtrip();
    failures += test_decoder_submit_oversized();
    failures += test_decoder_destroy_null();
    failures += test_encoder_destroy_null();
    failures += test_decoder_poll_empty();
    failures += test_decoder_flush_empty();
    failures += test_encoder_get_size_null();
    failures += test_decode_from_encoded();
    failures += test_encoder_get_bitrate();
    failures += test_encoder_set_bitrate();
    failures += test_encoder_set_bitrate_clamp();
    failures += test_encode_after_bitrate_change();
    failures += test_bitrate_rapid_changes();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures;
}
