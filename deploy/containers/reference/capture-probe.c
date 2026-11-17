/* validated on rootless Podman 4.9.3 / Ubuntu 24.04 aarch64; see video-validation.md.
 * Local qualification harness, not a replacement host or supported executable.
 * Link against the SAME libmetadesk.so shipped to the image. No fake backend.
 */
#include "core/capture.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    MdCaptureConfig cfg = {.target_fps = 15, .show_cursor = true};
    MdCaptureCtx *ctx = md_capture_create(&cfg);
    if (!ctx) return 2;
    double start = now();
    int rc = md_capture_start(ctx);
    printf("start_rc=%d elapsed=%.3f\n", rc, now() - start);
    if (rc) { md_capture_destroy(ctx); return 3; }
    /* Two distinct rendered backgrounds: verify pixels, not just callbacks.
     * No input injection, no encoder, no network, no mock capture backend. */
    const char *commands[] = {
        "swaymsg 'output HEADLESS-1 bg #204060 solid_color' >/dev/null",
        "swaymsg 'output HEADLESS-1 bg #a08020 solid_color' >/dev/null"
    };
    unsigned matches[2] = {0, 0}, frames = 0, timeouts = 0;
    const unsigned rgb[2][3] = {{0x20, 0x40, 0x60}, {0xa0, 0x80, 0x20}};
    for (unsigned change = 0; change < 10; change++) {
        unsigned phase = change % 2, before = matches[phase];
        if (system(commands[phase]) != 0) { md_capture_destroy(ctx); return 4; }
        double deadline = now() + 3, redraw = now() + .25;
        while (now() < deadline && matches[phase] == before) {
            if (now() >= redraw) {
                if (system(commands[phase]) != 0) { md_capture_destroy(ctx); return 4; }
                redraw = now() + .25;
            }
            MdFrame f = {0};
            if (md_capture_get_frame(ctx, &f)) {
                timeouts++;
                /* The backend can return immediately before STREAMING. This
                 * is retry pacing, never a success criterion or consent wait. */
                const struct timespec pause = {.tv_nsec = 10000000};
                nanosleep(&pause, NULL);
                continue;
            }
            frames++;
            size_t offset = (size_t)(f.height / 2) * f.stride + (f.width / 2) * 4;
            if (f.width != 1920 || f.height != 1080 || f.buf_type != MD_BUF_CPU ||
                !f.data || offset + 4 > f.data_size) {
                fprintf(stderr, "unexpected frame width=%u height=%u stride=%u "
                        "type=%d format=%d data_present=%d bytes=%zu seq=%u\n",
                        f.width, f.height, f.stride, f.buf_type, f.format,
                        f.data != NULL, f.data_size, f.seq);
                md_capture_release_frame(ctx, &f);
                md_capture_destroy(ctx);
                return 5;
            }
            const unsigned char *p = f.data + offset;
            int bgr = f.format == MD_PIX_CAPTURE_BGRX || f.format == MD_PIX_CAPTURE_BGRA;
            if (p[bgr ? 2 : 0] == rgb[phase][0] && p[1] == rgb[phase][1] &&
                p[bgr ? 0 : 2] == rgb[phase][2]) matches[phase]++;
            printf("phase=%u seq=%u size=%ux%u stride=%u format=%d bytes=%zu "
                   "pixel=%02x%02x%02x%02x timestamp=%" PRIu64 "\n",
                   phase, f.seq, f.width, f.height, f.stride, f.format, f.data_size,
                   p[0], p[1], p[2], p[3], f.timestamp_ns);
            md_capture_release_frame(ctx, &f);
        }
        if (matches[phase] == before) break;
    }
    /* A static, undamaged output can legitimately stop delivering new frames.
     * Measure that boundary separately from the changing-pixel success test. */
    if (matches[0] == 5 && matches[1] == 5) {
        unsigned idle_frames = 0, idle_timeouts = 0;
        double idle_start = now();
        for (unsigned i = 0; i < 3; i++) {
            MdFrame f = {0};
            if (md_capture_get_frame(ctx, &f)) idle_timeouts++;
            else { idle_frames++; md_capture_release_frame(ctx, &f); }
        }
        printf("static_frames=%u static_timeouts=%u elapsed=%.3f\n",
               idle_frames, idle_timeouts, now() - idle_start);
    }
    md_capture_stop(ctx);
    md_capture_destroy(ctx);
    printf("frames=%u timeouts=%u matching_pixels=%u,%u elapsed=%.3f\n",
           frames, timeouts, matches[0], matches[1], now() - start);
    return matches[0] >= 5 && matches[1] >= 5 ? 0 : 6;
}
