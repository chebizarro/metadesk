/*
 * metadesk — tools/capture_frame.c
 * Milestone 1.1 verification tool: capture a single frame to disk.
 *
 * Usage: ./capture_frame [output.raw [OUTPUT_SEL [MAX_WxH]]]
 *
 * OUTPUT_SEL is MdCaptureConfig.output (see capture.h / --capture-output);
 * "" selects the default. MAX_WxH is MdCaptureConfig.max_width/max_height
 * (host --capture-max-size; 0 = unbounded side).
 *
 * Captures one frame from the platform capture backend and writes
 * the raw pixel data to disk. Reports buffer type, dimensions,
 * format, latency (frame timestamp to receipt, on md_capture_clock_ns)
 * and the frame-pixels-per-output-unit scale.
 */
#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char *display = (argc > 2 && argv[2][0]) ? argv[2] : NULL;
    unsigned max_w = 0, max_h = 0;
    char trailing;
    if (argc > 3 && sscanf(argv[3], "%ux%u%c", &max_w, &max_h, &trailing) != 2) {
        fprintf(stderr, "ERROR: MAX_WxH must look like 1512x982 (0 = unbounded side)\n");
        return 1;
    }

    printf("metadesk capture_frame — M1.1 verification tool\n");
    printf("Output: %s\n", output_path);
    printf("Display: %s\n", display ? display : "(default)");
    if (max_w || max_h)
        printf("Max size: %ux%u\n", max_w, max_h);
    printf("\n");

    MdCaptureConfig cfg = {
        .target_fps  = 1, /* just need one frame */
        .show_cursor = true,
        .output      = display,
        .max_width   = max_w,
        .max_height  = max_h,
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
    /* Consent happened in start; allow time for format negotiation. */
    int ret = md_capture_get_frame(cap, &frame, 5000);
    if (ret != 0) {
        fprintf(stderr, "ERROR: no frame received.\n");
        md_capture_destroy(cap);
        return 1;
    }

    /* Signed: a timestamp after now would be a backend bug worth seeing. */
    const uint64_t received = md_capture_clock_ns();
    double latency_ms = 0.0;
    if (frame.timestamp_ns > 0)
        latency_ms = ((double)received - (double)frame.timestamp_ns) / 1e6;

    printf("Frame captured:\n");
    printf("  Dimensions: %ux%u (stride %u)\n",
           frame.width, frame.height, frame.stride);
    printf("  Format:     %s\n", pix_fmt_str(frame.format));
    printf("  Buffer:     %s\n", buf_type_str(frame.buf_type));
    printf("  Seq:        %u\n", frame.seq);
    printf("  Latency:    %.2f ms\n", latency_ms);
    MdCaptureRect rect;
    if (md_capture_get_output_rect(cap, &rect) == 0)
        printf("  Output:     %ux%u at (%d,%d), %.2fx%.2f frame px per unit\n",
               rect.width, rect.height, rect.x, rect.y,
               (double)frame.width / rect.width,
               (double)frame.height / rect.height);

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
