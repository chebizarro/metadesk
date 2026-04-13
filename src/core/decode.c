/*
 * metadesk — decode.c
 * FFmpeg H.264 decoding. Stub — implementation in M1.2.
 */
#include "decode.h"
#include <stdlib.h>

struct MdDecoder {
    int initialized;
};

MdDecoder *md_decoder_create(void) {
    MdDecoder *dec = calloc(1, sizeof(MdDecoder));
    /* TODO: FFmpeg decoder setup (M1.2) */
    return dec;
}

int md_decoder_submit(MdDecoder *dec, const uint8_t *data,
                      uint32_t size, uint64_t pts) {
    if (!dec || !data) return -1;
    (void)size; (void)pts;
    /* TODO: submit packet to decoder (M1.2) */
    return 0;
}

int md_decoder_poll(MdDecoder *dec, MdDecodeCallback cb, void *userdata) {
    if (!dec) return -1;
    (void)cb; (void)userdata;
    /* TODO: poll decoded frames (M1.2) */
    return 0;
}

void md_decoder_destroy(MdDecoder *dec) {
    free(dec);
}
