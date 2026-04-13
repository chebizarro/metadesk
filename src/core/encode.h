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
    bool     prefer_nvenc;  /* true = try NVENC first, false = x264   */
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
} MdEncodedPacket;

/* Callback invoked for each encoded packet. */
typedef void (*MdEncodeCallback)(const MdEncodedPacket *pkt, void *userdata);

/* Create encoder with given config. Returns NULL on failure.
 * Tries NVENC if prefer_nvenc is set, falls back to libx264. */
MdEncoder *md_encoder_create(const MdEncoderConfig *cfg);

/* Submit a raw frame for encoding.
 * data: pixel data in the format specified by input_fmt
 * stride: bytes per row (typically width * 4 for BGRX/RGBX)
 * input_fmt: pixel format of the input data
 * pts: presentation timestamp in microseconds
 * cb/userdata: callback invoked synchronously for each encoded packet
 *
 * The encoder converts to NV12 internally via libyuv.
 * Returns 0 on success, -1 on error. */
int md_encoder_submit(MdEncoder *enc, const uint8_t *data,
                      uint32_t stride, MdPixFmt input_fmt,
                      int64_t pts,
                      MdEncodeCallback cb, void *userdata);

/* Flush encoder — signal end of stream and retrieve remaining packets. */
int md_encoder_flush(MdEncoder *enc, MdEncodeCallback cb, void *userdata);

/* Query whether NVENC is being used (vs software fallback). */
bool md_encoder_is_hw(const MdEncoder *enc);

/* Get encoder dimensions (as negotiated at create time). */
int md_encoder_get_size(const MdEncoder *enc, uint32_t *width, uint32_t *height);

/* Destroy encoder and free resources. */
void md_encoder_destroy(MdEncoder *enc);

#ifdef __cplusplus
}
#endif

#endif /* MD_ENCODE_H */
