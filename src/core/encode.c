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
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libyuv.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#define MD_LOG_TAG "encode"

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
    if (!src || !dst_y || !dst_uv || width == 0 || height == 0 ||
        width > (uint32_t)INT_MAX || height > (uint32_t)INT_MAX ||
        stride > (uint32_t)INT_MAX)
        return -1;

    switch (fmt) {
    case MD_PIX_FMT_BGRX:
    case MD_PIX_FMT_BGRA:
        if ((size_t)stride < (size_t)width * 4u)
            return -1;
        /* libyuv: ARGBToNV12 expects BGRA/BGRx (it calls it "ARGB" in
         * little-endian convention: memory order B-G-R-A) */
        return ARGBToNV12(src, (int)stride,
                          dst_y, dst_stride_y,
                          dst_uv, dst_stride_uv,
                          (int)width, (int)height);

    case MD_PIX_FMT_RGBX:
    case MD_PIX_FMT_RGBA:
        if ((size_t)stride < (size_t)width * 4u)
            return -1;
        /* libyuv calls this "ABGR" in its naming convention */
        return ABGRToNV12(src, (int)stride,
                          dst_y, dst_stride_y,
                          dst_uv, dst_stride_uv,
                          (int)width, (int)height);

    case MD_PIX_FMT_NV12:
        /* Already NV12 — copy source planes row-by-row using the caller's
         * stride. Do not assume the source has FFmpeg's aligned linesizes. */
        if ((height % 2u) != 0 || stride < width ||
            dst_stride_y < (int)width || dst_stride_uv < (int)width)
            return -1;
        if ((size_t)height > SIZE_MAX / (size_t)stride)
            return -1;
        {
            for (uint32_t y = 0; y < height; y++) {
                uint8_t *dst_row = dst_y + (size_t)y * (size_t)dst_stride_y;
                memcpy(dst_row, src + (size_t)y * (size_t)stride, (size_t)width);
                if (dst_stride_y > (int)width) {
                    memset(dst_row + width, 0, (size_t)dst_stride_y - (size_t)width);
                }
            }

            const uint8_t *src_uv = src + (size_t)stride * (size_t)height;
            for (uint32_t y = 0; y < height / 2u; y++) {
                uint8_t *dst_row = dst_uv + (size_t)y * (size_t)dst_stride_uv;
                memcpy(dst_row, src_uv + (size_t)y * (size_t)stride, (size_t)width);
                if (dst_stride_uv > (int)width) {
                    memset(dst_row + width, 128, (size_t)dst_stride_uv - (size_t)width);
                }
            }
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
    uint32_t br = md_encoder_get_bitrate(enc);

    ctx->width     = (int)w;
    ctx->height    = (int)h;
    ctx->time_base = (AVRational){1, (int)fps};
    ctx->framerate = (AVRational){(int)fps, 1};
    ctx->pix_fmt   = AV_PIX_FMT_NV12;
    ctx->bit_rate  = (int64_t)br;
    ctx->gop_size  = 0;           /* intra-refresh instead of keyframes */
    ctx->max_b_frames = 0;        /* no B-frames for low latency        */

    /* Codec-specific low-latency options (spec §9). Collected into a dict and
     * handed to avcodec_open2, which consumes the options it accepts. Anything
     * still in the dict afterwards was REJECTED by this FFmpeg build's codec
     * — which means the low-latency contract can't be honoured (e.g. NVENC
     * would fall back to B-frames + lookahead). Rather than silently open a
     * degraded encoder, we fail so the caller's cascade tries the next one. */
    AVDictionary *opts = NULL;
    if (strcmp(codec_name, "h264_nvenc") == 0) {
        av_dict_set(&opts, "preset",      "p1",  0);
        av_dict_set(&opts, "tune",        "ull", 0);
        av_dict_set(&opts, "rc",          "cbr", 0);
        av_dict_set(&opts, "zerolatency", "1",   0);
        av_dict_set(&opts, "b_adapt",     "0",   0);
        av_dict_set_int(&opts, "intra-refresh", 1, 0);
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    } else if (strcmp(codec_name, "h264_amf") == 0) {
        av_dict_set(&opts, "usage",   "ultralowlatency", 0);
        av_dict_set(&opts, "quality", "speed",           0);
        av_dict_set(&opts, "rc",      "cbr",             0);
        av_dict_set_int(&opts, "header_spacing", -1, 0); /* SPS/PPS per IDR */
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    } else if (strcmp(codec_name, "h264_videotoolbox") == 0) {
        av_dict_set(&opts, "realtime", "true", 0);
        av_dict_set(&opts, "allow_sw", "0",    0);
        av_dict_set(&opts, "profile",  "high", 0);
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    } else if (strcmp(codec_name, "libx264") == 0) {
        av_dict_set(&opts, "preset", "ultrafast",   0);
        av_dict_set(&opts, "tune",   "zerolatency", 0);
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->gop_size = (int)fps * 2; /* keyframe every 2 seconds */
    }

    /* Threading: single-threaded for lowest latency */
    ctx->thread_count = 1;

    int ret = avcodec_open2(ctx, codec, &opts);
    if (ret < 0) {
        av_dict_free(&opts);
        avcodec_free_context(&ctx);
        return -1;
    }

    if (av_dict_count(opts) > 0) {
        const AVDictionaryEntry *e = NULL;
        while ((e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)))
            MD_LOG_W("%s rejected low-latency option '%s'", codec_name, e->key);
        av_dict_free(&opts);
        avcodec_free_context(&ctx);
        return -1;
    }
    av_dict_free(&opts);

    enc->codec = codec;
    enc->ctx   = ctx;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

MdEncoder *md_encoder_create(const MdEncoderConfig *cfg) {
    if (!cfg || cfg->width == 0 || cfg->height == 0)
        return NULL;

    if (cfg->width > (uint32_t)INT_MAX || cfg->height > (uint32_t)INT_MAX ||
        cfg->fps > (uint32_t)INT_MAX)
        return NULL;

    /* Width and height must be even for NV12 */
    if (cfg->width % 2 != 0 || cfg->height % 2 != 0)
        return NULL;

    MdEncoder *enc = calloc(1, sizeof(MdEncoder));
    if (!enc) return NULL;

    enc->config = *cfg;

    /* Try hardware encoders first, fall back to x264 software.
     * Cascade: NVENC (Linux/Windows NVIDIA) → VideoToolbox (macOS) → x264 */
    if (cfg->prefer_nvenc) {
        if (try_open_encoder(enc, "h264_nvenc") == 0) {
            enc->is_hw = true;
        }
    }

    /* Try VideoToolbox on macOS */
    if (!enc->ctx) {
        if (try_open_encoder(enc, "h264_videotoolbox") == 0) {
            enc->is_hw = true;
        }
    }

    /* Try AMF on Windows/AMD */
    if (!enc->ctx) {
        if (try_open_encoder(enc, "h264_amf") == 0) {
            enc->is_hw = true;
        }
    }

    /* Software fallback */
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

    int fill_ret = av_image_fill_arrays(enc->frame->data, enc->frame->linesize,
                                        enc->nv12_buf, AV_PIX_FMT_NV12,
                                        (int)cfg->width, (int)cfg->height, 32);
    if (fill_ret < 0) {
        av_free(enc->nv12_buf);
        av_frame_free(&enc->frame);
        avcodec_free_context(&enc->ctx);
        free(enc);
        return NULL;
    }

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
    if (ret < 0) {
        enc->cb = NULL;
        enc->cb_userdata = NULL;
        return -1;
    }

    /* Set PTS. FFmpeg expects PTS in the codec's time_base; use the
     * caller's timestamp when provided, else fall back to frame count. */
    enc->frame->pts = (pts >= 0) ? pts : (int64_t)enc->frame_idx++;
    if (pts >= 0)
        enc->frame_idx = pts + 1;

    /* Send frame to encoder */
    ret = avcodec_send_frame(enc->ctx, enc->frame);
    if (ret < 0) {
        enc->cb = NULL;
        enc->cb_userdata = NULL;
        return -1;
    }

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
    if (ret < 0 && ret != AVERROR_EOF) {
        enc->cb = NULL;
        enc->cb_userdata = NULL;
        return -1;
    }

    /* Drain all remaining packets */
    ret = receive_packets(enc);

    enc->cb = NULL;
    enc->cb_userdata = NULL;
    return ret < 0 ? -1 : ret;
}

/* ── Dynamic bitrate adjustment ──────────────────────────────── */

/*
 * Clamp bitrate to safe bounds. The floor prevents the encoder from
 * starving (artifacts / codec errors), and the ceiling prevents
 * runaway bandwidth.
 */
static uint32_t clamp_bitrate(uint32_t br) {
    if (br < MD_ENCODER_MIN_BITRATE) return MD_ENCODER_MIN_BITRATE;
    if (br > MD_ENCODER_MAX_BITRATE) return MD_ENCODER_MAX_BITRATE;
    return br;
}

int md_encoder_set_bitrate(MdEncoder *enc, uint32_t new_bitrate) {
    if (!enc || !enc->ctx)
        return -1;

    uint32_t br = clamp_bitrate(new_bitrate);

    /* Runtime reconfiguration, codec-independent. bit_rate is the primary knob
     * every FFmpeg H.264 encoder reads; rc_max_rate + rc_buffer_size pin the
     * CBR envelope. The private "b" option is what NVENC/AMF re-read per frame;
     * encoders that don't expose it return AVERROR_OPTION_NOT_FOUND, which is
     * benign. Any other av_opt_set error means the change did NOT take effect,
     * so report it (bitrate_ctrl's AIMD loop assumes its output is applied). */
    enc->ctx->bit_rate       = (int64_t)br;
    enc->ctx->rc_max_rate    = (int64_t)br;
    enc->ctx->rc_buffer_size = (int)(br * 2);  /* ~2-second VBV buffer */

    if (enc->ctx->priv_data) {
        char br_str[32];
        snprintf(br_str, sizeof(br_str), "%u", br);
        int ret = av_opt_set(enc->ctx->priv_data, "b", br_str, 0);
        if (ret < 0 && ret != AVERROR_OPTION_NOT_FOUND) {
            MD_LOG_W("set_bitrate: av_opt_set(b=%s) failed: %d", br_str, ret);
            return -1;
        }
    }

    enc->config.bitrate = br;
    return 0;
}

uint32_t md_encoder_get_bitrate(const MdEncoder *enc) {
    if (!enc) return 0;
    return enc->config.bitrate
         ? enc->config.bitrate
         : MD_ENCODER_DEFAULT_BITRATE;
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
