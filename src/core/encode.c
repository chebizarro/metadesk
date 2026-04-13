/*
 * metadesk — encode.c
 * FFmpeg H.264 encoding pipeline with NVENC and libx264 fallback.
 *
 * Encoder configuration per spec §9:
 *   NVENC: p1 preset, ull tune, cbr RC, no B-frames, intra-refresh
 *   x264:  ultrafast preset, zerolatency tune
 *
 * Colorspace conversion:
 *   Input from PipeWire is typically BGRx (4 bytes/pixel).
 *   libyuv converts to NV12 for the encoder.
 *
 * The encoder maintains its own NV12 frame buffer and AVFrame.
 * Caller submits raw frames via md_encoder_submit(); the callback
 * receives encoded NAL units synchronously during submit.
 */
#include "encode.h"

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libyuv.h>

#include <stdlib.h>
#include <string.h>

struct MdEncoder {
    MdEncoderConfig      config;
    const AVCodec       *codec;
    AVCodecContext       *ctx;
    AVFrame             *frame;      /* reusable NV12 frame                    */
    AVPacket            *pkt;        /* reusable output packet                 */
    uint8_t             *nv12_buf;   /* NV12 plane data backing frame->data[]  */
    int                  nv12_size;  /* total NV12 buffer size                 */
    bool                 is_hw;      /* true if using NVENC                    */
    int64_t              frame_idx;  /* monotonic frame counter for pts        */

    /* Encode callback — set during submit, used in receive loop */
    MdEncodeCallback     cb;
    void                *cb_userdata;
};

/* ── Colorspace conversion helpers (libyuv) ──────────────────── */

/*
 * Convert a 4-byte-per-pixel input format to NV12 using libyuv.
 * NV12 layout: Y plane (width*height), UV interleaved plane (width*height/2).
 */
static int convert_to_nv12(const uint8_t *src, uint32_t stride,
                           MdPixFmt fmt, uint32_t width, uint32_t height,
                           uint8_t *dst_y, int dst_stride_y,
                           uint8_t *dst_uv, int dst_stride_uv) {
    switch (fmt) {
    case MD_PIX_FMT_BGRX:
    case MD_PIX_FMT_BGRA:
        /* libyuv: ARGBToNV12 expects BGRA/BGRx (it calls it "ARGB" in
         * little-endian convention: memory order B-G-R-A) */
        return ARGBToNV12(src, (int)stride,
                          dst_y, dst_stride_y,
                          dst_uv, dst_stride_uv,
                          (int)width, (int)height);

    case MD_PIX_FMT_RGBX:
    case MD_PIX_FMT_RGBA:
        /* libyuv calls this "ABGR" in its naming convention */
        return ABGRToNV12(src, (int)stride,
                          dst_y, dst_stride_y,
                          dst_uv, dst_stride_uv,
                          (int)width, (int)height);

    case MD_PIX_FMT_NV12:
        /* Already NV12 — just copy planes */
        {
            int y_size = dst_stride_y * (int)height;
            int uv_size = dst_stride_uv * ((int)height / 2);
            memcpy(dst_y, src, (size_t)y_size);
            memcpy(dst_uv, src + y_size, (size_t)uv_size);
        }
        return 0;
    }
    return -1;
}

/* ── Receive encoded packets from the codec ──────────────────── */

static int receive_packets(MdEncoder *enc) {
    int count = 0;

    while (1) {
        int ret = avcodec_receive_packet(enc->ctx, enc->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return -1;

        /* Deliver to callback */
        if (enc->cb) {
            MdEncodedPacket out = {
                .data   = enc->pkt->data,
                .size   = (size_t)enc->pkt->size,
                .pts    = enc->pkt->pts,
                .dts    = enc->pkt->dts,
                .is_key = (enc->pkt->flags & AV_PKT_FLAG_KEY) != 0,
            };
            enc->cb(&out, enc->cb_userdata);
        }
        count++;
        av_packet_unref(enc->pkt);
    }

    return count;
}

/* ── Try opening a specific encoder ──────────────────────────── */

static int try_open_encoder(MdEncoder *enc, const char *codec_name) {
    const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec)
        return -1;

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return -1;

    uint32_t w = enc->config.width;
    uint32_t h = enc->config.height;
    uint32_t fps = enc->config.fps ? enc->config.fps : MD_ENCODER_DEFAULT_FPS;
    uint32_t br = enc->config.bitrate ? enc->config.bitrate : MD_ENCODER_DEFAULT_BITRATE;

    ctx->width     = (int)w;
    ctx->height    = (int)h;
    ctx->time_base = (AVRational){1, (int)fps};
    ctx->framerate = (AVRational){(int)fps, 1};
    ctx->pix_fmt   = AV_PIX_FMT_NV12;
    ctx->bit_rate  = (int64_t)br;
    ctx->gop_size  = 0;           /* intra-refresh instead of keyframes */
    ctx->max_b_frames = 0;        /* no B-frames for low latency        */

    /* Codec-specific options */
    if (strcmp(codec_name, "h264_nvenc") == 0) {
        /* NVENC: spec §9 parameters */
        av_opt_set(ctx->priv_data, "preset",        "p1",  0);
        av_opt_set(ctx->priv_data, "tune",          "ull", 0);
        av_opt_set(ctx->priv_data, "rc",            "cbr", 0);
        av_opt_set(ctx->priv_data, "zerolatency",   "1",   0);
        av_opt_set(ctx->priv_data, "b_adapt",       "0",   0);
        av_opt_set_int(ctx->priv_data, "intra-refresh", 1, 0);

        /* Tell FFmpeg we're okay with delay=0 output */
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    } else if (strcmp(codec_name, "libx264") == 0) {
        /* x264 software fallback: ultrafast + zerolatency */
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune",   "zerolatency", 0);

        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

        /* x264 needs gop_size > 0; use a large value with intra-refresh */
        ctx->gop_size = (int)fps * 2; /* keyframe every 2 seconds */
    }

    /* Threading: single-threaded for lowest latency */
    ctx->thread_count = 1;

    int ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
        avcodec_free_context(&ctx);
        return -1;
    }

    enc->codec = codec;
    enc->ctx   = ctx;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

MdEncoder *md_encoder_create(const MdEncoderConfig *cfg) {
    if (!cfg || cfg->width == 0 || cfg->height == 0)
        return NULL;

    /* Width and height must be even for NV12 */
    if (cfg->width % 2 != 0 || cfg->height % 2 != 0)
        return NULL;

    MdEncoder *enc = calloc(1, sizeof(MdEncoder));
    if (!enc) return NULL;

    enc->config = *cfg;

    /* Try NVENC first if requested, fall back to x264 */
    if (cfg->prefer_nvenc) {
        if (try_open_encoder(enc, "h264_nvenc") == 0) {
            enc->is_hw = true;
        }
    }

    if (!enc->ctx) {
        if (try_open_encoder(enc, "libx264") < 0) {
            free(enc);
            return NULL;
        }
        enc->is_hw = false;
    }

    /* Allocate reusable AVFrame for NV12 input */
    enc->frame = av_frame_alloc();
    if (!enc->frame) {
        avcodec_free_context(&enc->ctx);
        free(enc);
        return NULL;
    }

    enc->frame->format = AV_PIX_FMT_NV12;
    enc->frame->width  = (int)cfg->width;
    enc->frame->height = (int)cfg->height;

    /* Allocate NV12 buffer and assign to frame planes */
    enc->nv12_size = av_image_get_buffer_size(AV_PIX_FMT_NV12,
                                               (int)cfg->width, (int)cfg->height, 32);
    if (enc->nv12_size <= 0) {
        av_frame_free(&enc->frame);
        avcodec_free_context(&enc->ctx);
        free(enc);
        return NULL;
    }

    enc->nv12_buf = av_malloc((size_t)enc->nv12_size);
    if (!enc->nv12_buf) {
        av_frame_free(&enc->frame);
        avcodec_free_context(&enc->ctx);
        free(enc);
        return NULL;
    }

    av_image_fill_arrays(enc->frame->data, enc->frame->linesize,
                         enc->nv12_buf, AV_PIX_FMT_NV12,
                         (int)cfg->width, (int)cfg->height, 32);

    /* Allocate reusable output packet */
    enc->pkt = av_packet_alloc();
    if (!enc->pkt) {
        av_free(enc->nv12_buf);
        av_frame_free(&enc->frame);
        avcodec_free_context(&enc->ctx);
        free(enc);
        return NULL;
    }

    return enc;
}

int md_encoder_submit(MdEncoder *enc, const uint8_t *data,
                      uint32_t stride, MdPixFmt input_fmt,
                      int64_t pts,
                      MdEncodeCallback cb, void *userdata) {
    if (!enc || !data || !enc->ctx)
        return -1;

    enc->cb = cb;
    enc->cb_userdata = userdata;

    /* Convert input pixels to NV12 via libyuv */
    int ret = convert_to_nv12(data, stride, input_fmt,
                              (uint32_t)enc->ctx->width,
                              (uint32_t)enc->ctx->height,
                              enc->frame->data[0], enc->frame->linesize[0],
                              enc->frame->data[1], enc->frame->linesize[1]);
    if (ret < 0)
        return -1;

    /* Set PTS. FFmpeg expects PTS in the codec's time_base. */
    enc->frame->pts = enc->frame_idx++;

    /* Send frame to encoder */
    ret = avcodec_send_frame(enc->ctx, enc->frame);
    if (ret < 0)
        return -1;

    /* Receive any available encoded packets */
    ret = receive_packets(enc);

    enc->cb = NULL;
    enc->cb_userdata = NULL;

    if (ret < 0)
        return -1;

    return 0;
}

int md_encoder_flush(MdEncoder *enc, MdEncodeCallback cb, void *userdata) {
    if (!enc || !enc->ctx)
        return -1;

    enc->cb = cb;
    enc->cb_userdata = userdata;

    /* Send NULL frame to signal end of stream */
    int ret = avcodec_send_frame(enc->ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF)
        return -1;

    /* Drain all remaining packets */
    ret = receive_packets(enc);

    enc->cb = NULL;
    enc->cb_userdata = NULL;
    return ret < 0 ? -1 : ret;
}

bool md_encoder_is_hw(const MdEncoder *enc) {
    return enc ? enc->is_hw : false;
}

int md_encoder_get_size(const MdEncoder *enc, uint32_t *width, uint32_t *height) {
    if (!enc || !width || !height)
        return -1;
    *width  = enc->config.width;
    *height = enc->config.height;
    return 0;
}

void md_encoder_destroy(MdEncoder *enc) {
    if (!enc) return;

    if (enc->pkt)
        av_packet_free(&enc->pkt);
    if (enc->nv12_buf)
        av_free(enc->nv12_buf);
    if (enc->frame)
        av_frame_free(&enc->frame);
    if (enc->ctx)
        avcodec_free_context(&enc->ctx);

    free(enc);
}
