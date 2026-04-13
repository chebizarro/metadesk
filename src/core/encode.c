/*
 * metadesk — encode.c
 * FFmpeg NVENC encoding. Stub — implementation in M1.2.
 */
#include "encode.h"
#include <stdlib.h>
#include <stdbool.h>

struct MdEncoder {
    MdEncoderConfig config;
};

MdEncoder *md_encoder_create(const MdEncoderConfig *cfg) {
    if (!cfg) return NULL;
    MdEncoder *enc = calloc(1, sizeof(MdEncoder));
    if (enc)
        enc->config = *cfg;
    /* TODO: FFmpeg AVCodecContext setup (M1.2) */
    return enc;
}

int md_encoder_submit(MdEncoder *enc, const uint8_t *data,
                      uint32_t width, uint32_t height, uint64_t pts) {
    if (!enc || !data) return -1;
    (void)width; (void)height; (void)pts;
    /* TODO: submit frame to encoder (M1.2) */
    return 0;
}

int md_encoder_poll(MdEncoder *enc, MdEncodeCallback cb, void *userdata) {
    if (!enc) return -1;
    (void)cb; (void)userdata;
    /* TODO: poll encoded packets (M1.2) */
    return 0;
}

void md_encoder_destroy(MdEncoder *enc) {
    free(enc);
}
