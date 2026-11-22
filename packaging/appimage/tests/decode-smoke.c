/* SPDX-License-Identifier: MIT
 * Exercise the shipped metadesk decoder, including YUV -> RGBA conversion.
 * Input is a one-frame 16x16 Annex-B H.264 fixture, not a runtime encoder.
 */
#include "decode.h"
#include <stdio.h>
#include <stdlib.h>
static int frames, valid;
static void frame(const MdDecodedFrame *f, void *unused) {
    (void)unused;
    frames++;
    valid = f->width == 16 && f->height == 16 && f->stride == 64 && f->data && f->data[3] == 255;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    unsigned char data[4096];
    size_t n = fread(data, 1, sizeof(data), f);
    int too_large = fgetc(f) != EOF;
    fclose(f);
    if (!n || too_large) return 2;
    MdDecoder *d = md_decoder_create();
    if (!d) return 1;
    int rc = md_decoder_submit(d, data, n, 7);
    if (rc == 0) rc = md_decoder_poll(d, frame, NULL);
    if (rc >= 0) rc = md_decoder_flush(d, frame, NULL);
    md_decoder_destroy(d);
    if (rc < 0 || frames != 1 || !valid) return 1;
    puts("PASS: packaged metadesk H.264 -> 16x16 RGBA frame");
    return 0;
}
