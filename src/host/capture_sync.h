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

    /* Geometry last applied to input: frame size and output rectangle
     * (width 0 when the backend publishes none). */
    uint32_t      width, height;
    MdCaptureRect rect;

    /* An encoder size that failed to open: not retried for every frame,
     * only after the geometry changes again. */
    uint32_t failed_w, failed_h;
    uint32_t resizes;            /* successful encoder resizes */
    uint32_t bounds_updates;     /* input retargets */

    atomic_uint bitrate_req;     /* 0 = none */
    atomic_bool keyframe_req;
} HostCaptureSync;

/* width/height/rect: the geometry the encoder and input were created with
 * (rect may be NULL when none was published). */
void host_capture_sync_init(HostCaptureSync *s, MdEncoder *encoder,
                            MdInput *input, pthread_mutex_t *input_mu,
                            uint32_t width, uint32_t height,
                            const MdCaptureRect *rect);

/* Any thread. Applied by the capture thread before its next frame. */
void host_capture_sync_request_keyframe(HostCaptureSync *s);
void host_capture_sync_request_bitrate(HostCaptureSync *s, uint32_t bitrate);

/* Capture thread, for every frame get_frame returned: reconcile the input
 * bounds and the encoder with this frame and cap's published output
 * rectangle, then apply pending requests. Returns true when the frame
 * matches the encoder and may be submitted, false to drop it. */
bool host_capture_sync_frame(HostCaptureSync *s, const MdCaptureCtx *cap,
                             const MdFrame *frame);

#endif /* MD_HOST_CAPTURE_SYNC_H */
