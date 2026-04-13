/*
 * metadesk — decode.h
 * FFmpeg H.264 decoding pipeline.
 */
#ifndef MD_DECODE_H
#define MD_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque decoder context */
typedef struct MdDecoder MdDecoder;

/* Decoded frame delivered to callback */
typedef struct {
    uint8_t *data;       /* RGBA pixel data    */
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t pts;
} MdDecodedFrame;

typedef void (*MdDecodeCallback)(const MdDecodedFrame *frame, void *userdata);

/* Create decoder. Returns NULL on failure. */
MdDecoder *md_decoder_create(void);

/* Submit an encoded packet for decoding. */
int md_decoder_submit(MdDecoder *dec, const uint8_t *data,
                      uint32_t size, uint64_t pts);

/* Poll for decoded frames. Calls cb for each available frame. */
int md_decoder_poll(MdDecoder *dec, MdDecodeCallback cb, void *userdata);

/* Destroy decoder and free resources. */
void md_decoder_destroy(MdDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* MD_DECODE_H */
