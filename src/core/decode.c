/*
 * metadesk — decode.c
 * FFmpeg H.264 decoding pipeline with libyuv YUV → RGBA conversion.
 *
 * Receives H.264 NAL units, decodes to YUV, converts to RGBA for display.
 * Uses software decoding (libavcodec h264 decoder). Hardware decode
 * can be added later if needed, but software is fine for the client
 * side (spec §9.1: <5ms target for software decode on laptop).
 *
 * The decoder auto-detects frame dimensions from the bitstream.
 * RGBA output buffer is managed internally and resized as needed.
 */
#include "decode.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libyuv.h>

#include <stdlib.h>
#include <string.h>

struct MdDecoder {
    const AVCodec    *codec;
    AVCodecContext   *ctx;
    AVFrame          *frame;       /* reusable decoded YUV frame             */
    AVPacket         *pkt;         /* reusable input packet                  */

    /* RGBA output buffer (resized on dimension change) */
    uint8_t          *rgba_buf;
    uint32_t          rgba_width;
    uint32_t          rgba_height;
    uint32_t          rgba_stride;
    size_t            rgba_size;
};

/* ── YUV → RGBA conversion ───────────────────────────────────── */

/*
 * Convert a decoded AVFrame (NV12 or I420) to RGBA via libyuv.
 * Returns 0 on success.
 */
static int frame_to_rgba(const AVFrame *frame, uint8_t *dst, uint32_t dst_stride) {
    int w = frame->width;
    int h = frame->height;

    switch (frame->format) {
    case AV_PIX_FMT_NV12:
        return NV12ToARGB(frame->data[0], frame->linesize[0],   /* Y  */
                          frame->data[1], frame->linesize[1],   /* UV */
                          dst, (int)dst_stride,
                          w, h);

    case AV_PIX_FMT_YUV420P:
        return I420ToARGB(frame->data[0], frame->linesize[0],   /* Y  */
                          frame->data[1], frame->linesize[1],   /* U  */
                          frame->data[2], frame->linesize[2],   /* V  */
                          dst, (int)dst_stride,
                          w, h);

    case AV_PIX_FMT_NV21:
        return NV21ToARGB(frame->data[0], frame->linesize[0],
                          frame->data[1], frame->linesize[1],
                          dst, (int)dst_stride,
                          w, h);

    default:
        /* Unsupported pixel format from decoder */
        return -1;
    }
}

/* ── Ensure RGBA buffer is large enough ──────────────────────── */

static int ensure_rgba_buf(MdDecoder *dec, uint32_t width, uint32_t height) {
    if (dec->rgba_width == width && dec->rgba_height == height && dec->rgba_buf)
        return 0;

    uint32_t stride = width * 4;
    size_t size = (size_t)stride * height;

    uint8_t *buf = realloc(dec->rgba_buf, size);
    if (!buf)
        return -1;

    dec->rgba_buf    = buf;
    dec->rgba_width  = width;
    dec->rgba_height = height;
    dec->rgba_stride = stride;
    dec->rgba_size   = size;
    return 0;
}

/* ── Receive decoded frames from the codec ───────────────────── */

static int receive_frames(MdDecoder *dec, MdDecodeCallback cb, void *userdata) {
    int count = 0;

    while (1) {
        int ret = avcodec_receive_frame(dec->ctx, dec->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return -1;

        uint32_t w = (uint32_t)dec->frame->width;
        uint32_t h = (uint32_t)dec->frame->height;

        /* Ensure output buffer */
        if (ensure_rgba_buf(dec, w, h) < 0) {
            av_frame_unref(dec->frame);
            return -1;
        }

        /* Convert YUV → RGBA */
        ret = frame_to_rgba(dec->frame, dec->rgba_buf, dec->rgba_stride);
        if (ret < 0) {
            av_frame_unref(dec->frame);
            return -1;
        }

        /* Deliver to callback */
        if (cb) {
            MdDecodedFrame out = {
                .data   = dec->rgba_buf,
                .width  = w,
                .height = h,
                .stride = dec->rgba_stride,
                .pts    = dec->frame->pts,
            };
            cb(&out, userdata);
        }

        count++;
        av_frame_unref(dec->frame);
    }

    return count;
}

/* ── Public API ──────────────────────────────────────────────── */

MdDecoder *md_decoder_create(void) {
    MdDecoder *dec = calloc(1, sizeof(MdDecoder));
    if (!dec) return NULL;

    /* Use the generic h264 decoder — FFmpeg selects the best available */
    dec->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!dec->codec) {
        free(dec);
        return NULL;
    }

    dec->ctx = avcodec_alloc_context3(dec->codec);
    if (!dec->ctx) {
        free(dec);
        return NULL;
    }

    /* Low-latency settings */
    dec->ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    dec->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    dec->ctx->thread_count = 1;

    int ret = avcodec_open2(dec->ctx, dec->codec, NULL);
    if (ret < 0) {
        avcodec_free_context(&dec->ctx);
        free(dec);
        return NULL;
    }

    dec->frame = av_frame_alloc();
    if (!dec->frame) {
        avcodec_free_context(&dec->ctx);
        free(dec);
        return NULL;
    }

    dec->pkt = av_packet_alloc();
    if (!dec->pkt) {
        av_frame_free(&dec->frame);
        avcodec_free_context(&dec->ctx);
        free(dec);
        return NULL;
    }

    return dec;
}

int md_decoder_submit(MdDecoder *dec, const uint8_t *data,
                      size_t size, int64_t pts) {
    if (!dec || !data || size == 0 || !dec->ctx)
        return -1;

    /* Create a properly reference-counted packet.
     * av_packet_from_data would take ownership; instead we use
     * av_new_packet (which allocates + copies) to keep the caller's
     * buffer ownership clean and avoid dangling pointer issues. */
    av_packet_unref(dec->pkt);

    int ret = av_new_packet(dec->pkt, (int)size);
    if (ret < 0)
        return -1;

    memcpy(dec->pkt->data, data, size);
    dec->pkt->pts = pts;
    dec->pkt->dts = pts;

    ret = avcodec_send_packet(dec->ctx, dec->pkt);
    av_packet_unref(dec->pkt);

    if (ret < 0)
        return -1;

    return 0;
}

int md_decoder_poll(MdDecoder *dec, MdDecodeCallback cb, void *userdata) {
    if (!dec || !dec->ctx)
        return -1;

    return receive_frames(dec, cb, userdata);
}

int md_decoder_flush(MdDecoder *dec, MdDecodeCallback cb, void *userdata) {
    if (!dec || !dec->ctx)
        return -1;

    /* Send NULL packet to signal end of stream */
    int ret = avcodec_send_packet(dec->ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF)
        return -1;

    return receive_frames(dec, cb, userdata);
}

void md_decoder_destroy(MdDecoder *dec) {
    if (!dec) return;

    if (dec->pkt)
        av_packet_free(&dec->pkt);
    if (dec->frame)
        av_frame_free(&dec->frame);
    if (dec->ctx)
        avcodec_free_context(&dec->ctx);

    free(dec->rgba_buf);
    free(dec);
}
