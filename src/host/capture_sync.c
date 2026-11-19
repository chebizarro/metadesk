/*
 * metadesk — capture_sync.c
 * See capture_sync.h.
 */
#include "capture_sync.h"

#include <stdio.h>
#include <string.h>

void host_capture_sync_init(HostCaptureSync *s, MdEncoder *encoder,
                            MdInput *input, pthread_mutex_t *input_mu,
                            uint32_t width, uint32_t height,
                            const MdCaptureRect *rect) {
    memset(s, 0, sizeof(*s));
    s->encoder  = encoder;
    s->input    = input;
    s->input_mu = input_mu;
    s->width    = width;
    s->height   = height;
    if (rect) s->rect = *rect;
    atomic_init(&s->bitrate_req, 0u);
    atomic_init(&s->keyframe_req, false);
}

void host_capture_sync_request_keyframe(HostCaptureSync *s) {
    atomic_store(&s->keyframe_req, true);
}

void host_capture_sync_request_bitrate(HostCaptureSync *s, uint32_t bitrate) {
    if (bitrate) atomic_store(&s->bitrate_req, bitrate);
}

static bool rect_equal(const MdCaptureRect *a, const MdCaptureRect *b) {
    return a->x == b->x && a->y == b->y &&
           a->width == b->width && a->height == b->height;
}

/* Returns true when the geometry changed. */
static bool sync_input(HostCaptureSync *s, uint32_t w, uint32_t h,
                       const MdCaptureRect *rect) {
    if (w == s->width && h == s->height && rect_equal(rect, &s->rect))
        return false;
    if (s->input) {
        const MdInputBounds b = {
            .screen_width = w, .screen_height = h,
            .output_x = rect->x, .output_y = rect->y,
            .output_width = rect->width, .output_height = rect->height,
        };
        if (s->input_mu) pthread_mutex_lock(s->input_mu);
        int rc = md_input_set_bounds(s->input, &b);
        if (s->input_mu) pthread_mutex_unlock(s->input_mu);
        if (rc != 0) return false;
        s->bounds_updates++;
    }
    if (rect->width)
        fprintf(stderr, "host: capture geometry %ux%u -> %ux%u on output "
                "%ux%u at (%d,%d)\n", s->width, s->height, w, h,
                rect->width, rect->height, rect->x, rect->y);
    else
        fprintf(stderr, "host: capture geometry %ux%u -> %ux%u\n",
                s->width, s->height, w, h);
    s->width  = w;
    s->height = h;
    s->rect   = *rect;
    return true;
}

/* NV12/H.264 need even sizes: an odd edge is cropped by one row/column at
 * encode time (md-b7j), while input keeps the true frame size. */
static bool sync_encoder(HostCaptureSync *s, uint32_t w, uint32_t h) {
    uint32_t enc_w = 0, enc_h = 0;
    if (md_encoder_get_size(s->encoder, &enc_w, &enc_h) != 0)
        return false;
    const uint32_t want_w = w & ~1u, want_h = h & ~1u;
    if (enc_w == want_w && enc_h == want_h) {
        s->failed_w = s->failed_h = 0;
        return true;
    }
    if (want_w == s->failed_w && want_h == s->failed_h)
        return false;
    if (md_encoder_resize(s->encoder, want_w, want_h) != 0) {
        fprintf(stderr, "host: WARNING: cannot re-open the encoder at %ux%u; "
                "dropping %ux%u frames (encoder stays %ux%u)\n",
                want_w, want_h, w, h, enc_w, enc_h);
        s->failed_w = want_w;
        s->failed_h = want_h;
        return false;
    }
    s->failed_w = s->failed_h = 0;
    s->resizes++;
    fprintf(stderr, "host: encoder re-opened %ux%u -> %ux%u (%s); next frame "
            "is a keyframe\n", enc_w, enc_h, want_w, want_h,
            md_encoder_is_hw(s->encoder) ? "hardware" : "x264");
    return true;
}

bool host_capture_sync_frame(HostCaptureSync *s, const MdCaptureCtx *cap,
                             const MdFrame *frame) {
    if (!s || !frame ||
        frame->width < MD_INPUT_MIN_SCREEN_DIMENSION ||
        frame->height < MD_INPUT_MIN_SCREEN_DIMENSION)
        return false;

    /* A backend republishes its rectangle on (re)start; none is all zero. */
    MdCaptureRect rect = {0};
    if (md_capture_get_output_rect(cap, &rect) != 0)
        memset(&rect, 0, sizeof(rect));
    /* A failed size is retried once the geometry changes again. */
    if (sync_input(s, frame->width, frame->height, &rect))
        s->failed_w = s->failed_h = 0;

    if (!s->encoder)
        return false;
    const bool ok = sync_encoder(s, frame->width, frame->height);

    const uint32_t br = atomic_exchange(&s->bitrate_req, 0u);
    if (br) md_encoder_set_bitrate(s->encoder, br);
    if (atomic_exchange(&s->keyframe_req, false))
        md_encoder_force_keyframe(s->encoder);
    return ok;
}
