/*
 * metadesk — tools/capture_frame.c
 * Milestone 1.1 verification tool: capture a single PipeWire frame to disk.
 *
 * Usage: ./capture_frame [output.raw]
 *
 * Captures one frame from the PipeWire ScreenCast source and writes
 * the raw pixel data to disk. Reports buffer type (DMA-BUF vs SHM),
 * dimensions, format, and latency.
 */
#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

static volatile int g_got_frame = 0;
static const char *g_output_path = "frame.raw";

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void on_frame(const MdFrame *frame, void *userdata) {
    (void)userdata;

    if (g_got_frame)
        return; /* only capture the first frame */

    uint64_t capture_time = now_ns();
    double latency_ms = 0.0;
    if (frame->timestamp_ns > 0)
        latency_ms = (double)(capture_time - frame->timestamp_ns) / 1e6;

    printf("Frame captured:\n");
    printf("  Dimensions: %ux%u (stride %u)\n",
           frame->width, frame->height, frame->stride);
    printf("  Format:     0x%08x\n", frame->format);
    printf("  Buffer:     %s\n",
           frame->buf_type == MD_BUF_DMABUF ? "DMA-BUF" : "SHM");
    printf("  Seq:        %u\n", frame->seq);
    printf("  Latency:    %.2f ms\n", latency_ms);

    if (frame->buf_type == MD_BUF_SHM && frame->data && frame->data_size > 0) {
        FILE *f = fopen(g_output_path, "wb");
        if (f) {
            size_t written = fwrite(frame->data, 1, frame->data_size, f);
            fclose(f);
            printf("  Written:    %zu bytes → %s\n", written, g_output_path);
        } else {
            fprintf(stderr, "  ERROR: cannot open %s for writing\n", g_output_path);
        }
    } else if (frame->buf_type == MD_BUF_DMABUF) {
        printf("  DMA-BUF fd: %d (not written to disk — GPU-only buffer)\n",
               frame->dmabuf_fd);
        printf("  NOTE: Use M1.2 encode pipeline to read DMA-BUF on GPU\n");
    }

    g_got_frame = 1;
}

int main(int argc, char **argv) {
    if (argc > 1)
        g_output_path = argv[1];

    printf("metadesk capture_frame — M1.1 verification tool\n");
    printf("Output: %s\n\n", g_output_path);

    MdCaptureConfig cfg = {
        .target_fps    = 1, /* just need one frame */
        .prefer_dmabuf = true,
        .show_cursor   = true,
    };

    MdCapture *cap = md_capture_create(&cfg);
    if (!cap) {
        fprintf(stderr, "ERROR: failed to create capture context.\n");
        fprintf(stderr, "Is PipeWire running? (check: pw-cli info)\n");
        return 1;
    }

    printf("Starting capture (waiting for PipeWire stream)...\n");
    if (md_capture_start(cap, on_frame, NULL) < 0) {
        fprintf(stderr, "ERROR: failed to start capture.\n");
        md_capture_destroy(cap);
        return 1;
    }

    /* Wait for one frame (timeout after 10 seconds) */
    for (int i = 0; i < 100 && !g_got_frame; i++) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
        nanosleep(&ts, NULL);
    }

    if (!g_got_frame) {
        fprintf(stderr, "ERROR: no frame received after 10 seconds.\n");
        fprintf(stderr, "Check: is a ScreenCast source available?\n");
    }

    md_capture_destroy(cap);
    printf("Done.\n");
    return g_got_frame ? 0 : 1;
}
