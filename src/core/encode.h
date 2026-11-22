/*
 * metadesk — encode.h
 * FFmpeg H.264 encoding pipeline (NVENC with libx264 fallback).
 * See spec §9 for encoder configuration.
 *
 * Pipeline:
 *   1. Caller submits captured frames (BGRx/RGBx/RGBA from PipeWire)
 *   2. libyuv converts to NV12 (encoder input format)
 *   3. FFmpeg encodes via NVENC (or libx264 fallback)
 *   4. Encoded NAL units delivered to callback
 *
 * Thread safety: submit and poll may be called from the same thread.
 * The encoder is NOT thread-safe across threads — callers must
 * synchronize externally if needed.
 */
#ifndef MD_ENCODE_H
#define MD_ENCODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque encoder context */
typedef struct MdEncoder MdEncoder;

/* Input pixel format (matches PipeWire capture output) */
typedef enum {
    MD_PIX_FMT_BGRX,    /* 4 bytes/pixel, B-G-R-X (PipeWire default) */
    MD_PIX_FMT_RGBX,    /* 4 bytes/pixel, R-G-B-X                    */
    MD_PIX_FMT_RGBA,    /* 4 bytes/pixel, R-G-B-A                    */
    MD_PIX_FMT_BGRA,    /* 4 bytes/pixel, B-G-R-A                    */
    MD_PIX_FMT_NV12,    /* already NV12, skip conversion              */
} MdPixFmt;

/* Encoder configuration — spec §9 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;       /* bits/sec, default 8_000_000 (8 Mbps)   */
    uint32_t fps;           /* frames/sec, default 60                  */
    bool     prefer_nvenc;  /* try NVENC first (see md_encoder_create)  */
} MdEncoderConfig;

/* Default encoder settings */
#define MD_ENCODER_DEFAULT_BITRATE   8000000
#define MD_ENCODER_DEFAULT_FPS       60

/* Encoded packet delivered to callback */
typedef struct {
    const uint8_t *data;    /* NAL unit data (owned by encoder, valid until next call) */
    size_t         size;
    int64_t        pts;     /* presentation timestamp (encoder timebase)               */
    int64_t        dts;     /* decode timestamp                                        */
    bool           is_key;  /* true if IDR / keyframe                                  */
    uint64_t       tag;     /* the tag its frame was submitted with; 0 if unknown      */
} MdEncodedPacket;

/* Callback invoked for each encoded packet. */
typedef void (*MdEncodeCallback)(const MdEncodedPacket *pkt, void *userdata);

/* Create encoder with given config. Returns NULL on failure.
 *
 * Codec selection cascades through the first that opens AND accepts the spec
 * §9 low-latency options: [NVENC if prefer_nvenc] -> VideoToolbox -> AMF ->
 * libx264. prefer_nvenc only controls whether NVENC is attempted first; the
 * hardware backends for the current platform are always tried before the
 * libx264 software fallback. md_encoder_is_hw() reports which kind was chosen. */
MdEncoder *md_encoder_create(const MdEncoderConfig *cfg);

/* Submit a raw frame for encoding.
 * data: pixel data in the format specified by input_fmt
 * stride: bytes per row (typically width * 4 for BGRX/RGBX)
 * input_fmt: pixel format of the input data
 * pts: presentation timestamp in the codec timebase (1/fps), or < 0 for the
 *      frame count. The codec sees strictly increasing timestamps: a pts
 *      not after the previous frame's is advanced to the next one.
 * cb/userdata: callback invoked synchronously for each encoded packet
 *
 * The encoder converts to NV12 internally via libyuv.
 * Returns 0 on success, -1 on error. */
int md_encoder_submit(MdEncoder *enc, const uint8_t *data,
                      uint32_t stride, MdPixFmt input_fmt,
                      int64_t pts,
                      MdEncodeCallback cb, void *userdata);

/* md_encoder_submit, with per-frame metadata: the packet that encodes this
 * frame carries tag (MdEncodedPacket.tag) whenever the codec emits it. An
 * asynchronous codec (VideoToolbox, AMF) can emit it from a later submit or
 * the flush, after the caller has moved on; the tag still names this frame.
 * Packets are matched to frames by timestamp, and the codec holds only a
 * few frames, so a packet whose frame is no longer held carries tag 0.
 * md_encoder_submit submits with tag 0. */
int md_encoder_submit_tagged(MdEncoder *enc, const uint8_t *data,
                             uint32_t stride, MdPixFmt input_fmt,
                             int64_t pts, uint64_t tag,
                             MdEncodeCallback cb, void *userdata);

/* Flush encoder — signal end of stream and retrieve remaining packets. */
int md_encoder_flush(MdEncoder *enc, MdEncodeCallback cb, void *userdata);

/* Request that the next submitted frame be encoded as a keyframe (IDR).
 * Use when a new client connects to a persistent encoder so it gets an
 * immediate decodable entry point instead of waiting for the next GOP. */
void md_encoder_force_keyframe(MdEncoder *enc);

/* Re-open the encoder at a new size, e.g. when the captured display changes
 * resolution mid-stream. The same codec cascade as md_encoder_create runs
 * with the current bitrate and fps. Width and height must be even and
 * non-zero. Packets the old codec still buffered are discarded (with their
 * tags): they predate the change, and the next submitted frame is an IDR
 * whose in-band SPS carries the new size, so decoders re-initialise from it. Returns 0
 * (also for an unchanged size), or -1 with the encoder left as it was.
 * Same threading rule as md_encoder_submit. */
int md_encoder_resize(MdEncoder *enc, uint32_t width, uint32_t height);

/* Selected FFmpeg encoder name (e.g. h264_nvenc, h264_videotoolbox,
 * h264_amf, libx264). Borrowed static string; NULL for a NULL encoder. */
const char *md_encoder_get_name(const MdEncoder *enc);

/* Query whether a hardware backend is being used (vs software fallback). */
bool md_encoder_is_hw(const MdEncoder *enc);

/* Get the current encoder dimensions (create time, or the last resize). */
int md_encoder_get_size(const MdEncoder *enc, uint32_t *width, uint32_t *height);

/* Bitrate bounds for dynamic adjustment */
#define MD_ENCODER_MIN_BITRATE    100000      /* 100 Kbps floor  */
#define MD_ENCODER_MAX_BITRATE    100000000   /* 100 Mbps ceiling */

/*
 * Dynamically adjust the encoder's target bitrate without recreating
 * the codec context. Takes effect on the next submitted frame.
 *
 * For NVENC:        sets bit_rate + rc params via av_opt_set
 * For VideoToolbox: sets bit_rate on the codec context
 * For x264:         triggers encoder reconfiguration via FFmpeg
 *
 * new_bitrate: target bitrate in bits/sec. Clamped to
 *              [MD_ENCODER_MIN_BITRATE, MD_ENCODER_MAX_BITRATE].
 *
 * Returns 0 on success, -1 on error (NULL encoder, codec doesn't
 * support runtime reconfig, etc.).
 */
int md_encoder_set_bitrate(MdEncoder *enc, uint32_t new_bitrate);

/* Query the current target bitrate (bits/sec). */
uint32_t md_encoder_get_bitrate(const MdEncoder *enc);

/* Destroy encoder and free resources. */
void md_encoder_destroy(MdEncoder *enc);

#ifdef __cplusplus
}
#endif

#endif /* MD_ENCODE_H */
