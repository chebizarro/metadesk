/*
 * metadesk — tools/encode_roundtrip.c
 * M1.2 verification tool: encode/decode round-trip on localhost.
 *
 * Generates a test pattern, encodes with NVENC (or x264 fallback),
 * decodes, and optionally displays in an SDL2 window.
 *
 * Usage: encode_roundtrip [--frames N] [--no-display] [--width W] [--height H]
 *
 * Reports per-frame encode/decode latency and total round-trip time.
 */
#include "encode.h"
#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* ── Timing helpers ──────────────────────────────────────────── */

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ── Test pattern generation ─────────────────────────────────── */

/*
 * Generate a moving colour-bar pattern in BGRx format.
 * The pattern shifts each frame to visually confirm round-trip.
 */
static void generate_test_pattern(uint8_t *buf, uint32_t w, uint32_t h,
                                  uint32_t stride, int frame_num) {
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = buf + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t offset = x + (uint32_t)frame_num * 4;
            uint8_t *px = row + x * 4;
            /* Simple colour bars shifted by frame number */
            uint32_t bar = (offset / 32) % 8;
            px[0] = (bar & 1) ? 255 : 0;   /* B */
            px[1] = (bar & 2) ? 255 : 0;   /* G */
            px[2] = (bar & 4) ? 255 : 0;   /* R */
            px[3] = 255;                     /* X */
        }
    }
}

/* ── Round-trip state ────────────────────────────────────────── */

typedef struct {
    MdDecoder       *dec;
    int              decoded_count;
    int64_t          decode_start_us;
    int64_t          decode_total_us;
} RoundtripCtx;

/* Encode callback: forward encoded packet to decoder */
static void on_encoded(const MdEncodedPacket *pkt, void *userdata) {
    RoundtripCtx *rt = userdata;

    rt->decode_start_us = now_us();

    int ret = md_decoder_submit(rt->dec, pkt->data, pkt->size, pkt->pts);
    if (ret < 0) {
        fprintf(stderr, "  decoder submit failed\n");
        return;
    }
}

/* Decode callback: count and time */
static void on_decoded(const MdDecodedFrame *frame, void *userdata) {
    RoundtripCtx *rt = userdata;
    int64_t elapsed = now_us() - rt->decode_start_us;
    rt->decode_total_us += elapsed;
    rt->decoded_count++;
    (void)frame; /* in no-display mode we just count */
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    uint32_t width  = 1920;
    uint32_t height = 1080;
    int num_frames  = 60;
    bool no_display = true; /* SDL2 display deferred to client bead */

    /* Simple arg parsing */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            num_frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            width = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            height = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-display") == 0)
            no_display = true;
    }

    /* Ensure even dimensions */
    width  &= ~1u;
    height &= ~1u;

    printf("metadesk encode/decode round-trip test\n");
    printf("  resolution: %ux%u\n", width, height);
    printf("  frames:     %d\n", num_frames);
    printf("\n");

    /* Create encoder */
    MdEncoderConfig enc_cfg = {
        .width       = width,
        .height      = height,
        .bitrate     = MD_ENCODER_DEFAULT_BITRATE,
        .fps         = MD_ENCODER_DEFAULT_FPS,
        .prefer_nvenc = true,
    };

    MdEncoder *enc = md_encoder_create(&enc_cfg);
    if (!enc) {
        fprintf(stderr, "ERROR: failed to create encoder\n");
        return 1;
    }

    printf("  encoder:    %s\n", md_encoder_is_hw(enc) ? "NVENC (h264_nvenc)" : "x264 (libx264)");

    /* Create decoder */
    MdDecoder *dec = md_decoder_create();
    if (!dec) {
        fprintf(stderr, "ERROR: failed to create decoder\n");
        md_encoder_destroy(enc);
        return 1;
    }

    /* Allocate test pattern buffer */
    uint32_t stride = width * 4;
    uint8_t *pattern = malloc((size_t)stride * height);
    if (!pattern) {
        fprintf(stderr, "ERROR: OOM allocating pattern buffer\n");
        md_decoder_destroy(dec);
        md_encoder_destroy(enc);
        return 1;
    }

    RoundtripCtx rt = {
        .dec = dec,
        .decoded_count = 0,
        .decode_total_us = 0,
    };

    int64_t total_encode_us = 0;
    int64_t total_start = now_us();

    printf("\n  frame  encode_ms  decode_ms  key\n");
    printf("  ─────  ─────────  ─────────  ───\n");

    for (int i = 0; i < num_frames; i++) {
        /* Generate test pattern */
        generate_test_pattern(pattern, width, height, stride, i);

        /* Encode */
        int64_t enc_start = now_us();
        int ret = md_encoder_submit(enc, pattern, stride, MD_PIX_FMT_BGRX,
                                    (int64_t)i, on_encoded, &rt);
        int64_t enc_elapsed = now_us() - enc_start;
        total_encode_us += enc_elapsed;

        if (ret < 0) {
            fprintf(stderr, "  frame %d: encode failed\n", i);
            continue;
        }

        /* Poll decoder for output */
        int prev_count = rt.decoded_count;
        md_decoder_poll(dec, on_decoded, &rt);
        bool got_frame = (rt.decoded_count > prev_count);

        if (got_frame || (i % 10 == 0)) {
            printf("  %5d  %7.2f    %7.2f    %s\n",
                   i,
                   (double)enc_elapsed / 1000.0,
                   got_frame ? (double)(now_us() - rt.decode_start_us) / 1000.0 : 0.0,
                   got_frame ? "." : "-");
        }
    }

    /* Flush remaining */
    md_encoder_flush(enc, on_encoded, &rt);
    md_decoder_flush(dec, on_decoded, &rt);

    int64_t total_elapsed = now_us() - total_start;

    printf("\n── Summary ─────────────────────────────────\n");
    printf("  frames encoded:    %d\n", num_frames);
    printf("  frames decoded:    %d\n", rt.decoded_count);
    printf("  avg encode:        %.2f ms\n",
           (double)total_encode_us / num_frames / 1000.0);
    printf("  avg decode:        %.2f ms\n",
           rt.decoded_count > 0
           ? (double)rt.decode_total_us / rt.decoded_count / 1000.0 : 0.0);
    printf("  total wall time:   %.1f ms\n", (double)total_elapsed / 1000.0);
    printf("  throughput:        %.1f fps\n",
           (double)num_frames / ((double)total_elapsed / 1000000.0));

    if (rt.decoded_count < num_frames) {
        printf("\n  WARNING: %d frames lost in pipeline\n",
               num_frames - rt.decoded_count);
    }

    free(pattern);
    md_decoder_destroy(dec);
    md_encoder_destroy(enc);

    (void)no_display;
    return 0;
}
