/*
 * metadesk — encode.h
 * FFmpeg NVENC H.264 encoding pipeline.
 * See spec §9 for encoder configuration.
 */
#ifndef MD_ENCODE_H
#define MD_ENCODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque encoder context */
typedef struct MdEncoder MdEncoder;

/* Encoder configuration */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;       /* bits/sec, default 8000000  */
    uint32_t fps;           /* frames/sec, default 60     */
    bool     use_nvenc;     /* true = NVENC, false = x264 */
} MdEncoderConfig;

/* Encoded packet delivered to callback */
typedef struct {
    uint8_t *data;
    uint32_t size;
    uint64_t pts;
    bool     is_key;
} MdEncodedPacket;

typedef void (*MdEncodeCallback)(const MdEncodedPacket *pkt, void *userdata);

/* Create encoder with given config. Returns NULL on failure. */
MdEncoder *md_encoder_create(const MdEncoderConfig *cfg);

/* Submit a frame for encoding. Data is NV12 or I420. */
int md_encoder_submit(MdEncoder *enc, const uint8_t *data,
                      uint32_t width, uint32_t height, uint64_t pts);

/* Poll for encoded output. Calls cb for each available packet. */
int md_encoder_poll(MdEncoder *enc, MdEncodeCallback cb, void *userdata);

/* Destroy encoder and free resources. */
void md_encoder_destroy(MdEncoder *enc);

#ifdef __cplusplus
}
#endif

#endif /* MD_ENCODE_H */
