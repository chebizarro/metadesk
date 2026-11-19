/*
 * metadesk — capture_sync.h
 * Keeps the host's encoder and input bounds in step with the capture.
 *
 * The capture thread owns the encoder: it is the only thread that submits,
 * resizes, retunes or keys it, so a resize can replace the codec without a
 * lock. Other threads ask for a keyframe or a bitrate through the request
 * calls, which the capture thread applies before its next frame.
 *
 * Geometry is checked off every captured frame, not polled: when the frame
 * size or the capture's published output rectangle differs from what was
 * last applied (a watchdog restart, DXGI access-loss re-duplication, a mode
 * change, rotation, display rearrangement or a new "all" layout), the input
 * bounds are retargeted under the caller's input lock and the encoder is
 * re-opened at the new even size, which forces an IDR carrying the new SPS.
 *
 * Until the client decodes that IDR (about one RTT) it still maps pointer
 * input onto the old frame. Every MD_PKT_INPUT names the frame its point was
 * mapped onto (md-383x): its decoded size and the frame-channel sequence of
 * its video packet. Each geometry records the sequence the host's counter
 * stood at when it took effect, so host_capture_sync_map_point attributes a
 * point to the geometry its frame was sent under: used as-is for the current
 * one, rescaled for the previous one when that showed the same output
 * rectangle at another size, and dropped otherwise. A point is never
 * interpreted against a frame other than the one the client showed.
 */
#ifndef MD_HOST_CAPTURE_SYNC_H
#define MD_HOST_CAPTURE_SYNC_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "capture.h"
#include "encode.h"
#include "input.h"

typedef struct {
    MdEncoder       *encoder;    /* may be NULL */
    MdInput         *input;      /* may be NULL */
    pthread_mutex_t *input_mu;   /* serializes md_input_* calls; may be NULL */
    /* The frame-channel sequence counter: the next packet's sequence, which
     * every send advances under input_mu. Without it (NULL) no frame can be
     * attributed to a geometry, and every point is dropped. */
    const uint32_t  *packet_seq;

    /* Geometry last applied to input: frame size and output rectangle
     * (width 0 when the backend publishes none). Written only by the
     * capture thread, under input_mu; other threads read it under input_mu. */
    uint32_t      width, height;
    MdCaptureRect rect;
    /* The geometry before the last change (prev_width 0: none), for points
     * the client mapped onto a frame it was still showing. Each geometry's
     * frames went out with sequences from its first_seq on. */
    uint32_t      prev_width, prev_height;
    MdCaptureRect prev_rect;
    uint32_t      first_seq, prev_first_seq;

    /* An encoder size that failed to open: not retried for every frame,
     * only after the geometry changes again. */
    uint32_t failed_w, failed_h;
    uint32_t resizes;            /* successful encoder resizes */
    uint32_t bounds_updates;     /* input retargets */

    /* Pointer points mapped by host_capture_sync_map_point, under input_mu. */
    uint32_t points_translated;  /* rescaled from the previous frame */
    uint32_t points_dropped;     /* no safe mapping onto the current frame */
    bool     drop_logged;        /* one warning per geometry */

    atomic_uint bitrate_req;     /* 0 = none */
    atomic_bool keyframe_req;
} HostCaptureSync;

/* width/height/rect: the geometry the encoder and input were created with
 * (rect may be NULL when none was published). packet_seq: see above. */
void host_capture_sync_init(HostCaptureSync *s, MdEncoder *encoder,
                            MdInput *input, pthread_mutex_t *input_mu,
                            const uint32_t *packet_seq,
                            uint32_t width, uint32_t height,
                            const MdCaptureRect *rect);

/* Under input_mu, when the sequence counter restarts for a new stream:
 * everything that stream will see is sent under the current geometry. */
void host_capture_sync_reset_sequence(HostCaptureSync *s);

/* Any thread. Applied by the capture thread before its next frame. */
void host_capture_sync_request_keyframe(HostCaptureSync *s);
void host_capture_sync_request_bitrate(HostCaptureSync *s, uint32_t bitrate);

/* Capture thread, for every frame get_frame returned: reconcile the input
 * bounds and the encoder with this frame and cap's published output
 * rectangle, then apply pending requests. Returns true when the frame
 * matches the encoder and may be submitted, false to drop it. */
bool host_capture_sync_frame(HostCaptureSync *s, const MdCaptureCtx *cap,
                             const MdFrame *frame);

typedef enum {
    HOST_POINT_CURRENT    = 0, /* mapped against the current frame */
    HOST_POINT_TRANSLATED = 1, /* rescaled from the previous frame */
    HOST_POINT_DROPPED    = 2, /* unknown or unsafe geometry: do not inject */
} HostPointMap;

/* Under input_mu, like every md_input_* call. Maps a pointer point (x, y)
 * the client computed on the decoded frame_w x frame_h frame of video
 * packet frame_pkt_seq onto the frame the input bounds follow now, into
 * out_x and out_y.
 *
 * frame_pkt_seq picks the geometry the frame was sent under: the current one
 * from first_seq on, the previous one from prev_first_seq on; a sequence
 * older than both, or not sent yet, is dropped. The tagged size must then
 * equal that geometry's even-rounded size (the encoder crops an odd edge;
 * md-b7j) and the point must lie inside it. A current-geometry point is used
 * as-is. A previous-geometry point is rescaled onto the current frame only
 * when both show the same published output rectangle (a pure rescale, e.g.
 * a HiDPI or downscale change), so it lands on the same desktop point; with
 * no published rectangle, or another one (mode change, rotation,
 * rearrangement, new layout), the content under it may have moved and it is
 * dropped. */
HostPointMap host_capture_sync_map_point(HostCaptureSync *s,
                                         uint16_t frame_w, uint16_t frame_h,
                                         uint32_t frame_pkt_seq, int x, int y,
                                         int *out_x, int *out_y);

#endif /* MD_HOST_CAPTURE_SYNC_H */
