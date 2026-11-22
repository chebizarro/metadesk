/* SPDX-License-Identifier: MIT
 * Runtime inventory gate: verify-codecs ENCODER [ENCODER...]
 * Link against the just-built avcodec/avutil, not the producer's system FFmpeg.
 */
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: verify-codecs ENCODER [ENCODER...]\n");
        return 2;
    }
    printf("License: %s\n", avcodec_license());
    printf("Configuration: %s\n", avcodec_configuration());
    if (strcmp(avcodec_license(), "LGPL version 2.1 or later") ||
        strcmp(avutil_license(), "LGPL version 2.1 or later"))
        return 1;
    void *state = NULL;
    const AVCodec *codec;
    int decoders = 0, encoders = 0, parsers = 0;
    while ((codec = av_codec_iterate(&state))) {
        int decoder = av_codec_is_decoder(codec);
        printf("%s: %s\n", decoder ? "decoder" : "encoder", codec->name);
        if (decoder) {
            if (strcmp(codec->name, "h264")) return 1;
            decoders++;
        } else {
            int found = 0;
            for (int i = 1; i < argc; i++)
                if (!strcmp(codec->name, argv[i])) found = 1;
            if (!found || !av_codec_is_encoder(codec)) return 1;
            encoders++;
        }
    }
    for (int i = 1; i < argc; i++)
        if (!avcodec_find_encoder_by_name(argv[i])) return 1;
    state = NULL;
    const AVCodecParser *parser;
    while ((parser = av_parser_iterate(&state))) {
        if (parser->codec_ids[0] != AV_CODEC_ID_H264) return 1;
        puts("parser: h264");
        parsers++;
    }
    if (decoders != 1 || encoders != argc - 1 || parsers != 1) return 1;
    puts("PASS: exact minimal codec/parser inventory; LGPL-2.1-or-later");
    return 0;
}
