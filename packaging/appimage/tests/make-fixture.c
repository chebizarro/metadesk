/* SPDX-License-Identifier: MIT
 * Offline test-data generator; build with the distro FFmpeg, NEVER bundle it.
 * cc make-fixture.c $(pkg-config --cflags --libs libavcodec libavutil) -o make-fixture
 * ./make-fixture > frame.h264
 */
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) return 1;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    c->width = c->height = 16;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = (AVRational){1, 1};
    av_opt_set(c->priv_data, "preset", "ultrafast", 0);
    av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    if (avcodec_open2(c, codec, NULL) < 0) return 1;
    AVFrame *f = av_frame_alloc();
    f->format = c->pix_fmt; f->width = c->width; f->height = c->height;
    if (av_frame_get_buffer(f, 32) < 0) return 1;
    for (int plane = 0; plane < 3; plane++)
        for (int row = 0; row < (plane ? 8 : 16); row++)
            memset(f->data[plane] + row * f->linesize[plane], plane ? 128 : 64, plane ? 8 : 16);
    AVPacket *packet = av_packet_alloc();
    if (avcodec_send_frame(c, f) < 0 || avcodec_receive_packet(c, packet) < 0) return 1;
    int ok = fwrite(packet->data, 1, packet->size, stdout) == (size_t)packet->size;
    av_packet_free(&packet); av_frame_free(&f); avcodec_free_context(&c);
    return ok ? 0 : 1;
}
