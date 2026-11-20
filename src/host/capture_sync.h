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
 * its video packet. host_capture_sync_map_point attributes a point to the
 * geometry that frame was captured under: used as-is for the current one,
 * rescaled from an earlier one that showed the same output rectangle at
 * another size, and dropped otherwise. A point is never interpreted against
 * a frame other than the one the client showed.
 *
 * Attribution follows the capture frame, not the time a packet is sent
 * (md-f7uc). Each applied geometry is a generation; the capture thread
 * submits every frame tagged with the generation it was captured under
 * (host_capture_sync_frame_tag), the encoder hands the tag back with that
 * frame's packet whenever it emits it, and the sender reports each video
 * packet's tag and sequence (host_capture_sync_packet_sent). Consecutive
 * packets of one generation form a run; a point's sequence picks its run.
 * An asynchronous encoder (VideoToolbox, AMF) that emits a frame captured
 * before a change after the change was applied therefore still attributes
 * it to the old geometry, and a synchronous one behaves the same way.
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

/* A geometry applied to input. gen 0: unknown (no frame names it). */
typedef struct {
    uint32_t      gen;
    uint32_t      width, height;
    MdCaptureRect rect;          /* width 0 when the backend publishes none */
} HostGeometry;

/* Consecutive video packets of one generation, from first_seq up to the
 * next run's first_seq (the newest run: up to the counter). */
typedef struct {
    uint32_t     first_seq;
    HostGeometry geometry;       /* gen 0: frames of unknown geometry */
} HostPacketRun;

/* Generations a packet still in the encoder can name: far more geometry
 * changes than an encoder pipeline holds frames. */
#define HOST_CAPTURE_SYNC_GEOMETRIES 8u
/* Runs a point can be attributed to; older frames are dropped. */
#define HOST_CAPTURE_SYNC_RUNS       8u

typedef struct {
    MdEncoder       *encoder;    /* may be NULL */
    MdInput         *input;      /* may be NULL */
    pthread_mutex_t *input_mu;   /* serializes md_input_* calls; may be NULL */
    /* The frame-channel sequence counter: the next packet's sequence, which
     * every send advances under input_mu. Without it (NULL) no frame can be
     * attributed to a geometry, and every point is dropped. */
    const uint32_t  *packet_seq;

    /* Geometry last applied to input: frame size and output rectangle
     * (width 0 when the backend publishes none), and its generation (from
     * 1). Written only by the capture thread, under input_mu; other threads
     * read it under input_mu. geometries holds the recent generations by
     * gen % HOST_CAPTURE_SYNC_GEOMETRIES, same rule. */
    uint32_t      width, height;
    MdCaptureRect rect;
    uint32_t      generation;
    HostGeometry  geometries[HOST_CAPTURE_SYNC_GEOMETRIES];

    /* Video packets sent, as runs of one generation: the newest is
     * runs[(runs_total - 1) % HOST_CAPTURE_SYNC_RUNS]. Under input_mu. */
    HostPacketRun runs[HOST_CAPTURE_SYNC_RUNS];
    uint32_t      runs_total;

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

/* Under input_mu, when the sequence counter restarts for a new stream: no
 * packet of the old stream is attributable any more. */
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

/* Capture thread, after host_capture_sync_frame accepted a frame: the tag to
 * submit it with (md_encoder_submit_tagged), naming the generation of the
 * geometry it was captured under. Never 0. */
uint64_t host_capture_sync_frame_tag(const HostCaptureSync *s);

/* Under input_mu, for every video packet as it is given sequence seq:
 * frame_tag is the tag the encoder returned with it (MdEncodedPacket.tag).
 * Attributes the packet to the geometry its frame was captured under; an
 * unknown tag (0, or a generation no longer held) attributes it to none, so
 * points on it are dropped. */
void host_capture_sync_packet_sent(HostCaptureSync *s, uint64_t frame_tag,
                                   uint32_t seq);

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
 * frame_pkt_seq picks the run of packets it belongs to, and so the geometry
 * its frame was captured under; a sequence older than every run held, or not
 * sent yet, is dropped. The tagged size must then equal that geometry's
 * even-rounded size (the encoder crops an odd edge; md-b7j) and the point
 * must lie inside it. A current-geometry point is used as-is. An earlier
 * geometry's point is rescaled onto the current frame only when both show
 * the same published output rectangle (a pure rescale, e.g. a HiDPI or
 * downscale change), so it lands on the same desktop point; with no
 * published rectangle, or another one (mode change, rotation, rearrangement,
 * new layout), the content under it may have moved and it is dropped. */
HostPointMap host_capture_sync_map_point(HostCaptureSync *s,
                                         uint16_t frame_w, uint16_t frame_h,
                                         uint32_t frame_pkt_seq, int x, int y,
                                         int *out_x, int *out_y);

#endif /* MD_HOST_CAPTURE_SYNC_H */
