/*
 * metadesk — tools/capture_frame.c
 * Milestone 1.1 verification tool: capture a single frame to disk.
 *
 * Usage: ./capture_frame [output.raw]
 *
 * Captures one frame from the platform capture backend and writes
 * the raw pixel data to disk. Reports buffer type, dimensions,
 * format, and latency.
 */
#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static const char *buf_type_str(MdBufferType bt) {
    switch (bt) {
    case MD_BUF_CPU:    return "CPU (SHM)";
    case MD_BUF_DMABUF: return "DMA-BUF";
    case MD_BUF_GPU:    return "GPU";
    }
    return "unknown";
}

static const char *pix_fmt_str(MdCapturePixFmt fmt) {
    switch (fmt) {
    case MD_PIX_CAPTURE_BGRX: return "BGRx";
    case MD_PIX_CAPTURE_BGRA: return "BGRA";
    case MD_PIX_CAPTURE_RGBX: return "RGBx";
    case MD_PIX_CAPTURE_RGBA: return "RGBA";
    case MD_PIX_CAPTURE_NV12: return "NV12";
    }
    return "unknown";
}

int main(int argc, char **argv) {
    const char *output_path = (argc > 1) ? argv[1] : "frame.raw";

    printf("metadesk capture_frame — M1.1 verification tool\n");
    printf("Output: %s\n\n", output_path);

    MdCaptureConfig cfg = {
        .target_fps  = 1, /* just need one frame */
        .show_cursor = true,
    };

    MdCaptureCtx *cap = md_capture_create(&cfg);
    if (!cap) {
        fprintf(stderr, "ERROR: failed to create capture context.\n");
        fprintf(stderr, "Is the capture backend available?\n");
        return 1;
    }

    printf("Starting capture...\n");
    if (md_capture_start(cap) < 0) {
        fprintf(stderr, "ERROR: failed to start capture.\n");
        md_capture_destroy(cap);
        return 1;
    }

    /* Get one frame */
    MdFrame frame;
    int ret = md_capture_get_frame(cap, &frame);
    if (ret != 0) {
        fprintf(stderr, "ERROR: no frame received.\n");
        md_capture_destroy(cap);
        return 1;
    }

    uint64_t capture_time = now_ns();
    double latency_ms = 0.0;
    if (frame.timestamp_ns > 0)
        latency_ms = (double)(capture_time - frame.timestamp_ns) / 1e6;

    printf("Frame captured:\n");
    printf("  Dimensions: %ux%u (stride %u)\n",
           frame.width, frame.height, frame.stride);
    printf("  Format:     %s\n", pix_fmt_str(frame.format));
    printf("  Buffer:     %s\n", buf_type_str(frame.buf_type));
    printf("  Seq:        %u\n", frame.seq);
    printf("  Latency:    %.2f ms\n", latency_ms);

    if (frame.buf_type == MD_BUF_CPU && frame.data && frame.data_size > 0) {
        FILE *f = fopen(output_path, "wb");
        if (f) {
            size_t written = fwrite(frame.data, 1, frame.data_size, f);
            fclose(f);
            printf("  Written:    %zu bytes → %s\n", written, output_path);
        } else {
            fprintf(stderr, "  ERROR: cannot open %s for writing\n", output_path);
        }
    } else if (frame.buf_type == MD_BUF_DMABUF) {
        printf("  DMA-BUF fd: %d (not written to disk — GPU-only buffer)\n",
               frame.dmabuf_fd);
        printf("  NOTE: Use encode pipeline to read DMA-BUF on GPU\n");
    } else if (frame.buf_type == MD_BUF_GPU) {
        printf("  GPU handle:  %p (not written to disk)\n", frame.gpu_handle);
    }

    md_capture_release_frame(cap, &frame);
    md_capture_destroy(cap);
    printf("Done.\n");
    return 0;
}
