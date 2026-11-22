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
#include <limits.h>
#include "md_test.h"

/* Force a real backend without changing production fallback semantics. */
static MdEncoder *create_test_encoder(const MdEncoderConfig *config) {
    if (!config) return md_encoder_create(NULL);
    MdEncoderConfig cfg = *config;
    cfg.encoder_name = getenv("MD_TEST_ENCODER");
    return md_encoder_create(&cfg);
}
#define md_encoder_create create_test_encoder

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
    MD_CHECK(frame->width == TEST_W);
    MD_CHECK(frame->height == TEST_H);
    MD_CHECK(frame->stride == TEST_W * 4);
    MD_CHECK(frame->data != NULL);
}

/* ── Test: encoder creation ──────────────────────────────────── */

static int test_encoder_create(void) {
    printf("  test_encoder_create... ");

    /* NULL config should fail */
    MD_CHECK(md_encoder_create(NULL) == NULL);

    /* Zero dimensions should fail */
    MdEncoderConfig cfg = { .width = 0, .height = 480 };
    MD_CHECK(md_encoder_create(&cfg) == NULL);

    /* Odd dimensions should fail (NV12 requires even) */
    cfg.width = 641; cfg.height = 480;
    MD_CHECK(md_encoder_create(&cfg) == NULL);

    /* Valid config should succeed */
    cfg.width = TEST_W;
    cfg.height = TEST_H;
    cfg.bitrate = MD_ENCODER_DEFAULT_BITRATE;
    cfg.fps = 30;
    cfg.prefer_nvenc = true;

    MdEncoder *enc = md_encoder_create(&cfg);
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    uint32_t w, h;
    MD_CHECK(md_encoder_get_size(enc, &w, &h) == 0);
    MD_CHECK(w == TEST_W && h == TEST_H);

    const char *name = md_encoder_get_name(enc);
    MD_CHECK(name != NULL);
    MD_CHECK(strcmp(name, "openh264") == 0 || strcmp(name, "h264_mf") == 0 ||
             strcmp(name, "h264_vaapi") == 0 || strcmp(name, "h264_qsv") == 0 ||
             strcmp(name, "h264_videotoolbox") == 0 || strcmp(name, "h264_nvenc") == 0 ||
             strcmp(name, "h264_amf") == 0 || strcmp(name, "libx264") == 0);
    MD_CHECK(md_encoder_is_hw(enc) == (strcmp(name, "libx264") != 0 &&
             strcmp(name, "openh264") != 0 && strcmp(name, "h264_mf") != 0));
    MD_CHECK(md_encoder_get_name(NULL) == NULL);
    printf("OK (%s)\n", name);
    md_encoder_destroy(enc);

    cfg.prefer_nvenc = false;
    enc = md_encoder_create(&cfg);
    MD_CHECK(enc != NULL);
    if (!enc) return 1;
    name = md_encoder_get_name(enc);
    MD_CHECK(name != NULL);
    const char *forced = getenv("MD_TEST_ENCODER");
    MD_CHECK(forced ? strcmp(name, forced) == 0 : strcmp(name, "h264_nvenc") != 0);
    MD_CHECK(strcmp(name, "openh264") == 0 || strcmp(name, "h264_mf") == 0 ||
             strcmp(name, "h264_vaapi") == 0 || strcmp(name, "h264_qsv") == 0 ||
             strcmp(name, "h264_videotoolbox") == 0 || strcmp(name, "h264_nvenc") == 0 ||
             strcmp(name, "h264_amf") == 0 || strcmp(name, "libx264") == 0);
    MD_CHECK(md_encoder_is_hw(enc) == (strcmp(name, "libx264") != 0 &&
             strcmp(name, "openh264") != 0 && strcmp(name, "h264_mf") != 0));
    printf("  non-NVENC fallback: %s\n", name);
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
    MD_CHECK(md_encoder_create(&cfg) == NULL);

    cfg.width = 2;
    cfg.height = (uint32_t)INT_MAX + 1u;
    MD_CHECK(md_encoder_create(&cfg) == NULL);

    cfg.height = 2;
    cfg.fps = (uint32_t)INT_MAX + 1u;
    MD_CHECK(md_encoder_create(&cfg) == NULL);

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
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    uint8_t *buf = calloc(1, (size_t)cfg.width * cfg.height * 4u);
    MD_CHECK(buf != NULL);

    MD_CHECK(md_encoder_submit(enc, buf, cfg.width * 4u - 1u, MD_PIX_FMT_BGRX,
                             0, NULL, NULL) == -1);
    MD_CHECK(md_encoder_submit(enc, buf, cfg.width - 1u, MD_PIX_FMT_NV12,
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
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    MD_CHECK(buf != NULL);

    g_encoded_count = 0;

    /* Encode 10 frames */
    for (int i = 0; i < 10; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride,
                   (uint8_t)(i * 25), 128, 64);
        int ret = md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                    (int64_t)i, on_encode, NULL);
        MD_CHECK(ret == 0);
    }

    /* Flush remaining */
    md_encoder_flush(enc, on_encode, NULL);

    /* Should have gotten some encoded packets */
    MD_CHECK(g_encoded_count > 0);
    printf("OK (%d packets from 10 frames)\n", g_encoded_count);

    free(g_last_encoded_data);
    g_last_encoded_data = NULL;
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
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    MdDecoder *dec = md_decoder_create();
    MD_CHECK(dec != NULL);

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    MD_CHECK(buf != NULL);

    g_encoded_count = 0;
    g_decoded_count = 0;

    /* Encode 30 frames, feeding each encoded packet to decoder */
    for (int i = 0; i < 30; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride,
                   (uint8_t)((i * 8) % 256), (uint8_t)((i * 4) % 256), 100);

        free(g_last_encoded_data);
        g_last_encoded_data = NULL;
        g_last_encoded_size = 0;

        int ret = md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                    (int64_t)i, on_encode, NULL);
        MD_CHECK(ret == 0);

        /* If we got an encoded packet, submit to decoder */
        if (g_last_encoded_data && g_last_encoded_size > 0) {
            ret = md_decoder_submit(dec, g_last_encoded_data,
                                    g_last_encoded_size, (int64_t)i);
            MD_CHECK(ret == 0);

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
    MD_CHECK(g_decoded_count > 0);

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
    MD_CHECK(dec != NULL);

    /* Submit garbage data — should not crash, just return error */
    uint8_t garbage[16] = {0};
    int ret = md_decoder_submit(dec, garbage, sizeof(garbage), 0);
    /* Decoder may accept or reject garbage depending on codec state — just don't crash */
    (void)ret;

    /* Poll should return 0 frames (no valid input) */
    g_decoded_count = 0;
    md_decoder_poll(dec, on_decode, NULL);
    MD_CHECK(g_decoded_count == 0);

    printf("OK\n");
    md_decoder_destroy(dec);
    return 0;
}

/* ── Test: oversized decoder submissions are rejected ─────────── */

static int test_decoder_submit_oversized(void) {
    printf("  test_decoder_submit_oversized... ");

    MdDecoder *dec = md_decoder_create();
    MD_CHECK(dec != NULL);

    uint8_t byte = 0;
    MD_CHECK(md_decoder_submit(dec, &byte, (size_t)INT_MAX + 1u, 0) == -1);

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
    MD_CHECK(dec != NULL);

    g_decoded_count = 0;
    int nframes = md_decoder_poll(dec, on_decode, NULL);
    MD_CHECK(nframes == 0);
    MD_CHECK(g_decoded_count == 0);

    md_decoder_destroy(dec);
    printf("OK\n");
    return 0;
}

/* ── Test: decoder flush with no submissions ─────────────────── */

static int test_decoder_flush_empty(void) {
    printf("  test_decoder_flush_empty... ");

    MdDecoder *dec = md_decoder_create();
    MD_CHECK(dec != NULL);

    g_decoded_count = 0;
    int nframes = md_decoder_flush(dec, on_decode, NULL);
    /* Flush with no pending data should return 0 or succeed benignly */
    MD_CHECK(nframes >= 0);
    MD_CHECK(g_decoded_count == 0);

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
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    /* NULL output params */
    MD_CHECK(md_encoder_get_size(enc, NULL, NULL) == -1);
    uint32_t w;
    MD_CHECK(md_encoder_get_size(enc, &w, NULL) == -1);
    uint32_t h;
    MD_CHECK(md_encoder_get_size(enc, NULL, &h) == -1);

    /* Valid */
    MD_CHECK(md_encoder_get_size(enc, &w, &h) == 0);
    MD_CHECK(w == TEST_W && h == TEST_H);

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
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    MdDecoder *dec = md_decoder_create();
    MD_CHECK(dec != NULL);

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    MD_CHECK(buf != NULL);

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
            MD_CHECK(ret == 0);
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
    MD_CHECK(md_encoder_get_bitrate(NULL) == 0);

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 5000000,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    /* Should return the configured bitrate */
    MD_CHECK(md_encoder_get_bitrate(enc) == 5000000);

    md_encoder_destroy(enc);

    /* Zero bitrate in config → should return default */
    cfg.bitrate = 0;
    enc = md_encoder_create(&cfg);
    MD_CHECK(enc != NULL);
    if (!enc) return 1;
    MD_CHECK(md_encoder_get_bitrate(enc) == MD_ENCODER_DEFAULT_BITRATE);
    md_encoder_destroy(enc);

    printf("OK\n");
    return 0;
}

/* ── Test: set_bitrate basic ─────────────────────────────────── */

static int test_encoder_set_bitrate(void) {
    printf("  test_encoder_set_bitrate... ");

    /* NULL encoder should fail */
    MD_CHECK(md_encoder_set_bitrate(NULL, 4000000) == -1);

    MdEncoderConfig cfg = {
        .width = TEST_W,
        .height = TEST_H,
        .bitrate = 8000000,
        .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    /* Set a new bitrate */
    MD_CHECK(md_encoder_set_bitrate(enc, 4000000) == 0);
    MD_CHECK(md_encoder_get_bitrate(enc) == 4000000);

    /* Set again */
    MD_CHECK(md_encoder_set_bitrate(enc, 12000000) == 0);
    MD_CHECK(md_encoder_get_bitrate(enc) == 12000000);

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
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    /* Below minimum → clamped to MIN */
    MD_CHECK(md_encoder_set_bitrate(enc, 1000) == 0);
    MD_CHECK(md_encoder_get_bitrate(enc) == MD_ENCODER_MIN_BITRATE);

    /* Zero → clamped to MIN */
    MD_CHECK(md_encoder_set_bitrate(enc, 0) == 0);
    MD_CHECK(md_encoder_get_bitrate(enc) == MD_ENCODER_MIN_BITRATE);

    /* Above maximum → clamped to MAX */
    MD_CHECK(md_encoder_set_bitrate(enc, 500000000) == 0);
    MD_CHECK(md_encoder_get_bitrate(enc) == MD_ENCODER_MAX_BITRATE);

    /* Exactly at bounds */
    MD_CHECK(md_encoder_set_bitrate(enc, MD_ENCODER_MIN_BITRATE) == 0);
    MD_CHECK(md_encoder_get_bitrate(enc) == MD_ENCODER_MIN_BITRATE);

    MD_CHECK(md_encoder_set_bitrate(enc, MD_ENCODER_MAX_BITRATE) == 0);
    MD_CHECK(md_encoder_get_bitrate(enc) == MD_ENCODER_MAX_BITRATE);

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
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    MD_CHECK(buf != NULL);

    g_encoded_count = 0;

    /* Encode 5 frames at 8 Mbps */
    for (int i = 0; i < 5; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride, 200, 100, 50);
        MD_CHECK(md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                 (int64_t)i, on_encode, NULL) == 0);
    }
    int before = g_encoded_count;

    /* Change bitrate to 2 Mbps */
    MD_CHECK(md_encoder_set_bitrate(enc, 2000000) == 0);
    MD_CHECK(md_encoder_get_bitrate(enc) == 2000000);

    /* Encode 5 more frames at 2 Mbps */
    for (int i = 5; i < 10; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride, 50, 200, 100);
        MD_CHECK(md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                 (int64_t)i, on_encode, NULL) == 0);
    }

    /* Change bitrate to 16 Mbps */
    MD_CHECK(md_encoder_set_bitrate(enc, 16000000) == 0);
    MD_CHECK(md_encoder_get_bitrate(enc) == 16000000);

    /* Encode 5 more frames at 16 Mbps */
    for (int i = 10; i < 15; i++) {
        fill_solid(buf, TEST_W, TEST_H, stride, 100, 50, 200);
        MD_CHECK(md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                 (int64_t)i, on_encode, NULL) == 0);
    }

    md_encoder_flush(enc, on_encode, NULL);

    /* Should have encoded packets across all three bitrate settings */
    MD_CHECK(g_encoded_count > before);

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
    MD_CHECK(enc != NULL);
    if (!enc) return 1;

    uint32_t stride = TEST_W * 4;
    uint8_t *buf = malloc((size_t)stride * TEST_H);
    MD_CHECK(buf != NULL);

    g_encoded_count = 0;

    /* Change bitrate before every frame */
    uint32_t bitrates[] = {1000000, 8000000, 500000, 20000000, 3000000,
                           100000, 50000000, 2000000, 10000000, 4000000};

    for (int i = 0; i < 10; i++) {
        MD_CHECK(md_encoder_set_bitrate(enc, bitrates[i]) == 0);
        fill_solid(buf, TEST_W, TEST_H, stride,
                   (uint8_t)(i * 25), (uint8_t)(255 - i * 25), 128);
        MD_CHECK(md_encoder_submit(enc, buf, stride, MD_PIX_FMT_BGRX,
                                 (int64_t)i, on_encode, NULL) == 0);
    }

    md_encoder_flush(enc, on_encode, NULL);
    MD_CHECK(g_encoded_count > 0);

    printf("OK (%d packets, 10 bitrate changes)\n", g_encoded_count);

    free(buf);
    md_encoder_destroy(enc);
    return 0;
}

/* ── Test: per-frame tags through the codec pipeline (md-f7uc) ── */

/* Every packet carries the tag of the frame it encodes, whenever the codec
 * emits it: an asynchronous codec (VideoToolbox here on macOS) emits frames
 * from later submits or the flush. Timestamps that restart (a capture
 * restart) are advanced so frames stay distinguishable, and a resize
 * discards the frames the old codec held, with their tags. */

#define TAG_FRAMES  24
#define TAG_RESIZE  16   /* frames from here on are 320x240 */
#define TAG_BASE    1000u

typedef struct {
    int      submitted;           /* frames handed to the encoder */
    int64_t  pts[TAG_FRAMES];     /* each frame's expected codec pts */
    bool     seen[TAG_FRAMES];
    uint64_t last_tag;
    int      packets, late, untagged;
} TagLog;

static void on_tagged(const MdEncodedPacket *pkt, void *userdata) {
    TagLog *t = userdata;
    t->packets++;
    if (pkt->tag < TAG_BASE || pkt->tag >= TAG_BASE + (uint64_t)t->submitted) {
        t->untagged++;
        return;
    }
    const int frame = (int)(pkt->tag - TAG_BASE);
    MD_CHECK(pkt->pts == t->pts[frame]);  /* it encodes that frame */
    MD_CHECK(pkt->tag > t->last_tag);     /* in order, each once */
    MD_CHECK(!t->seen[frame]);
    t->seen[frame] = true;
    t->last_tag = pkt->tag;
    if (frame < t->submitted - 1)
        t->late++;                        /* emitted after later frames */
}

static int test_tags_follow_frames(void) {
    printf("  test_tags_follow_frames... ");
    MdEncoderConfig cfg = {
        .width = TEST_W, .height = TEST_H, .bitrate = 4000000, .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    MD_CHECK(enc != NULL);
    if (!enc) return 1;
    uint8_t *buf = malloc((size_t)TEST_W * 4u * TEST_H);
    MD_CHECK(buf != NULL);
    if (!buf) { md_encoder_destroy(enc); return 1; }

    TagLog t = { .last_tag = 0 };
    int64_t next = 0;                     /* the codec's next lowest pts */
    for (int i = 0; i < TAG_FRAMES; i++) {
        uint32_t w = TEST_W, h = TEST_H;
        if (i == TAG_RESIZE) {
            MD_CHECK(md_encoder_resize(enc, 320, 240) == 0);
        }
        if (i >= TAG_RESIZE) { w = 320; h = 240; }
        /* Gaps of two, then a restart of the caller's count at frame 10. */
        const int64_t caller = i < 10 ? 2 * i : i - 10;
        t.pts[i] = caller < next ? next : caller;
        next = t.pts[i] + 1;
        fill_solid(buf, w, h, w * 4u, (uint8_t)(i * 10), 80, (uint8_t)(255 - i * 10));
        t.submitted = i + 1;
        MD_CHECK(md_encoder_submit_tagged(enc, buf, w * 4u, MD_PIX_FMT_BGRX,
                                          caller, TAG_BASE + (uint64_t)i,
                                          on_tagged, &t) == 0);
    }
    MD_CHECK(md_encoder_flush(enc, on_tagged, &t) >= 0);

    MD_CHECK(t.untagged == 0);
    /* What the old codec held at the resize is gone; nothing else is. */
    int before = 0;
    while (before < TAG_RESIZE && t.seen[before]) before++;
    for (int i = before; i < TAG_RESIZE; i++) MD_CHECK(!t.seen[i]);
    MD_CHECK(before >= TAG_RESIZE - 4);
    for (int i = TAG_RESIZE; i < TAG_FRAMES; i++) MD_CHECK(t.seen[i]);
    MD_CHECK(t.packets == before + (TAG_FRAMES - TAG_RESIZE));

    /* md_encoder_submit is the untagged path. */
    g_encoded_count = 0;
    MdEncoder *plain = md_encoder_create(&cfg);
    MD_CHECK(plain != NULL);
    if (plain) {
        TagLog u = { .submitted = 0 };
        fill_solid(buf, TEST_W, TEST_H, TEST_W * 4u, 1, 2, 3);
        MD_CHECK(md_encoder_submit(plain, buf, TEST_W * 4u, MD_PIX_FMT_BGRX,
                                   0, on_tagged, &u) == 0);
        MD_CHECK(md_encoder_flush(plain, on_tagged, &u) >= 0);
        MD_CHECK(u.packets == 1 && u.untagged == 1);
        md_encoder_destroy(plain);
    }

    printf("OK (%s: %d packets, %d emitted after later frames, %d "
           "discarded at the resize)\n", md_encoder_get_name(enc), t.packets, t.late, TAG_RESIZE - before);
    free(buf);
    md_encoder_destroy(enc);
    return 0;
}

/* ── Persistent encoder across client reconnects (md-3nv) ────── */

typedef struct {
    MdDecoder *decoder;
    uint64_t first_tag;
    int packets, decoded;
} ReconnectClient;

static void on_reconnect_decode(const MdDecodedFrame *frame, void *userdata) {
    ReconnectClient *client = userdata;
    MD_CHECK(frame->data != NULL);
    MD_CHECK(frame->width == TEST_W && frame->height == TEST_H);
    client->decoded++;
}

static void on_reconnect_packet(const MdEncodedPacket *pkt, void *userdata) {
    ReconnectClient *client = userdata;
    /* An asynchronous encoder may still emit the departed client's frames. */
    if (pkt->tag < client->first_tag) return;
    if (client->packets++ == 0) {
        MD_CHECK(pkt->tag == client->first_tag);
        MD_CHECK(pkt->is_key);
    }
    MD_CHECK(md_decoder_submit(client->decoder, pkt->data, pkt->size, pkt->pts) == 0);
    MD_CHECK(md_decoder_poll(client->decoder, on_reconnect_decode, client) >= 0);
}

static int test_encoder_reuse_after_client_leaves(void) {
    printf("  test_encoder_reuse_after_client_leaves... ");
    int failures_before = atomic_load(&md_test_failures);
    MdEncoderConfig cfg = {
        .width = TEST_W, .height = TEST_H, .bitrate = 4000000, .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    MD_CHECK(enc != NULL);
    if (!enc) return 1;
    uint8_t *buf = malloc((size_t)TEST_W * 4u * TEST_H);
    MD_CHECK(buf != NULL);
    if (!buf) { md_encoder_destroy(enc); return 1; }
    fill_solid(buf, TEST_W, TEST_H, TEST_W * 4u, 40, 80, 120);

    for (int session = 0; session < 20; session++) {
        ReconnectClient client = {
            .decoder = md_decoder_create(),
            .first_tag = (uint64_t)session * 12u + 1u,
        };
        MD_CHECK(client.decoder != NULL);
        if (!client.decoder) { free(buf); md_encoder_destroy(enc); return 1; }
        md_encoder_force_keyframe(enc);
        for (int frame = 0; frame < 12; frame++) {
            MD_CHECK(md_encoder_submit_tagged(enc, buf, TEST_W * 4u,
                MD_PIX_FMT_BGRX, session * 12 + frame, client.first_tag + frame,
                on_reconnect_packet, &client) == 0);
        }
        /* Client teardown must not send EOF to the persistent encoder. The
         * next client must decode independently, including in-band headers. */
        MD_CHECK(md_decoder_flush(client.decoder, on_reconnect_decode, &client) >= 0);
        MD_CHECK(client.packets > 0);
        MD_CHECK(client.decoded > 0);
        md_decoder_destroy(client.decoder);
    }
    free(buf);
    md_encoder_destroy(enc);
    printf("%s (20 clients, independent decoders, no encoder EOF)\n",
           atomic_load(&md_test_failures) == failures_before ? "OK" : "FAILED");
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
    failures += test_tags_follow_frames();
    failures += test_encoder_reuse_after_client_leaves();

    md_test_failures += failures;
    printf("\n%s\n", atomic_load(&md_test_failures) == 0
           ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return md_test_report();
}
