/*
 * metadesk — capture_sync.c
 * See capture_sync.h.
 */
#include "capture_sync.h"

#include <stdio.h>
#include <string.h>

void host_capture_sync_init(HostCaptureSync *s, MdEncoder *encoder,
                            MdInput *input, pthread_mutex_t *input_mu,
                            const uint32_t *packet_seq,
                            uint32_t width, uint32_t height,
                            const MdCaptureRect *rect) {
    memset(s, 0, sizeof(*s));
    s->encoder    = encoder;
    s->input      = input;
    s->input_mu   = input_mu;
    s->packet_seq = packet_seq;
    s->first_seq  = packet_seq ? *packet_seq : 0;
    s->width    = width;
    s->height   = height;
    if (rect) s->rect = *rect;
    atomic_init(&s->bitrate_req, 0u);
    atomic_init(&s->keyframe_req, false);
}

void host_capture_sync_reset_sequence(HostCaptureSync *s) {
    s->first_seq = s->packet_seq ? *s->packet_seq : 0;
    s->prev_width = s->prev_height = 0;
    memset(&s->prev_rect, 0, sizeof(s->prev_rect));
    s->prev_first_seq = 0;
    s->drop_logged = false;
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

/* Returns true when the geometry changed. The bounds and the geometry that
 * host_capture_sync_map_point reads change together under input_mu. */
static bool sync_input(HostCaptureSync *s, uint32_t w, uint32_t h,
                       const MdCaptureRect *rect) {
    if (w == s->width && h == s->height && rect_equal(rect, &s->rect))
        return false;
    const uint32_t old_w = s->width, old_h = s->height;
    if (s->input_mu) pthread_mutex_lock(s->input_mu);
    if (s->input) {
        const MdInputBounds b = {
            .screen_width = w, .screen_height = h,
            .output_x = rect->x, .output_y = rect->y,
            .output_width = rect->width, .output_height = rect->height,
        };
        if (md_input_set_bounds(s->input, &b) != 0) {
            if (s->input_mu) pthread_mutex_unlock(s->input_mu);
            return false;
        }
        s->bounds_updates++;
    }
    s->prev_width  = s->width;
    s->prev_height = s->height;
    s->prev_rect   = s->rect;
    s->prev_first_seq = s->first_seq;
    s->first_seq   = s->packet_seq ? *s->packet_seq : 0;
    s->width  = w;
    s->height = h;
    s->rect   = *rect;
    s->drop_logged = false;
    if (s->input_mu) pthread_mutex_unlock(s->input_mu);

    if (rect->width)
        fprintf(stderr, "host: capture geometry %ux%u -> %ux%u on output "
                "%ux%u at (%d,%d)\n", old_w, old_h, w, h,
                rect->width, rect->height, rect->x, rect->y);
    else
        fprintf(stderr, "host: capture geometry %ux%u -> %ux%u\n",
                old_w, old_h, w, h);
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

/* The decoded size of a geometry: the encoder's even-rounded size. */
static bool size_matches(uint32_t w, uint32_t h,
                         uint16_t frame_w, uint16_t frame_h) {
    return w && h && frame_w == (w & ~1u) && frame_h == (h & ~1u);
}

/* Sequence order, modulo 2^32. */
static bool seq_at_or_after(uint32_t seq, uint32_t from) {
    return (int32_t)(seq - from) >= 0;
}

/* p in an n-wide frame onto the nearest pixel of an m-wide one showing the
 * same region: md_input_map_to_output maps both to within half an m-pixel
 * of the same desktop point. */
static int rescale(int p, uint32_t n, uint32_t m) {
    const uint64_t q = (2u * (uint64_t)p * m + n) / (2u * (uint64_t)n);
    return (int)(q < m ? q : m - 1u);
}

static HostPointMap map_point(const HostCaptureSync *s,
                              uint16_t frame_w, uint16_t frame_h,
                              uint32_t frame_pkt_seq, int x, int y,
                              int *out_x, int *out_y) {
    if (!s->packet_seq ||
        x < 0 || y < 0 || x >= (int)frame_w || y >= (int)frame_h)
        return HOST_POINT_DROPPED;
    /* A frame not sent yet is no frame the client showed. */
    if (seq_at_or_after(frame_pkt_seq, *s->packet_seq))
        return HOST_POINT_DROPPED;
    if (seq_at_or_after(frame_pkt_seq, s->first_seq)) {
        if (!size_matches(s->width, s->height, frame_w, frame_h))
            return HOST_POINT_DROPPED;
        *out_x = x;
        *out_y = y;
        return HOST_POINT_CURRENT;
    }
    if (!s->prev_width || !seq_at_or_after(frame_pkt_seq, s->prev_first_seq) ||
        !size_matches(s->prev_width, s->prev_height, frame_w, frame_h))
        return HOST_POINT_DROPPED;
    /* Same output rectangle at another size: both frames show the same
     * desktop region, so a rescale lands on the same desktop point. */
    if (!s->rect.width || !s->rect.height ||
        !rect_equal(&s->rect, &s->prev_rect))
        return HOST_POINT_DROPPED;
    *out_x = rescale(x, s->prev_width, s->width);
    *out_y = rescale(y, s->prev_height, s->height);
    return HOST_POINT_TRANSLATED;
}

HostPointMap host_capture_sync_map_point(HostCaptureSync *s,
                                         uint16_t frame_w, uint16_t frame_h,
                                         uint32_t frame_pkt_seq, int x, int y,
                                         int *out_x, int *out_y) {
    if (!s || !out_x || !out_y)
        return HOST_POINT_DROPPED;
    const HostPointMap m = map_point(s, frame_w, frame_h, frame_pkt_seq, x, y,
                                     out_x, out_y);
    if (m == HOST_POINT_TRANSLATED) {
        s->points_translated++;
    } else if (m == HOST_POINT_DROPPED) {
        s->points_dropped++;
        if (!s->drop_logged) {
            s->drop_logged = true;
            fprintf(stderr, "host: dropping pointer input on a %ux%u frame "
                    "(packet %u; frame now %ux%u from packet %u): stale or "
                    "unknown geometry\n", frame_w, frame_h, frame_pkt_seq,
                    s->width & ~1u, s->height & ~1u, s->first_seq);
        }
    }
    return m;
}
