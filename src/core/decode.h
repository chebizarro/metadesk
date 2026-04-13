/*
 * metadesk — decode.h
 * FFmpeg H.264 decoding pipeline.
 *
 * Pipeline:
 *   1. Caller submits encoded H.264 NAL units (from network)
 *   2. FFmpeg decodes to YUV (NV12 or I420)
 *   3. libyuv converts to RGBA (for SDL2 display)
 *   4. Decoded RGBA frames delivered to callback
 *
 * Thread safety: submit and poll may be called from the same thread.
 * The decoder is NOT thread-safe across threads.
 */
#ifndef MD_DECODE_H
#define MD_DECODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque decoder context */
typedef struct MdDecoder MdDecoder;

/* Decoded frame delivered to callback */
typedef struct {
    const uint8_t *data;     /* RGBA pixel data (owned by decoder)      */
    uint32_t       width;
    uint32_t       height;
    uint32_t       stride;   /* bytes per row (width * 4 for RGBA)      */
    int64_t        pts;      /* presentation timestamp from encoder     */
} MdDecodedFrame;

/* Callback invoked for each decoded frame. */
typedef void (*MdDecodeCallback)(const MdDecodedFrame *frame, void *userdata);

/* Create decoder. Codec is auto-detected from the bitstream.
 * Returns NULL on failure. */
MdDecoder *md_decoder_create(void);

/* Submit an encoded packet for decoding.
 * data: H.264 NAL unit(s)
 * size: byte count
 * pts: presentation timestamp (passed through to output)
 * Returns 0 on success, -1 on error. */
int md_decoder_submit(MdDecoder *dec, const uint8_t *data,
                      size_t size, int64_t pts);

/* Poll for decoded frames. Calls cb for each available frame.
 * Returns the number of frames delivered, or -1 on error. */
int md_decoder_poll(MdDecoder *dec, MdDecodeCallback cb, void *userdata);

/* Flush decoder — retrieve remaining buffered frames. */
int md_decoder_flush(MdDecoder *dec, MdDecodeCallback cb, void *userdata);

/* Destroy decoder and free resources. */
void md_decoder_destroy(MdDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* MD_DECODE_H */
