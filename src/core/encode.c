/* metadesk — platform H.264 encoders, runtime OpenH264, optional x264. */
#include "encode.h"
#include "encoder_chain.h"
#include "encode_openh264.h"
#include <libavutil/hwcontext.h>

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

/* Far deeper than any low-latency pipeline (0-3 frames): a frame the codec
 * holds longer has its tag forgotten, never misattributed. */
#define MD_ENCODER_INFLIGHT 32u

struct MdEncoder {
    MdEncoderConfig      config;
    char                *openh264_path, *encoder_name; /* owned config strings */
    MdOpenH264          *openh264;
    AVFrame             *hw_frame;
    const AVCodec       *codec;
    AVCodecContext       *ctx;
    AVFrame             *frame;      /* reusable NV12 frame                    */
    AVPacket            *pkt;        /* reusable output packet                 */
    bool                 is_hw;      /* true for any hardware backend          */
    bool                 force_keyframe; /* emit an IDR on the next frame       */
    int64_t              frame_idx;  /* the next frame's lowest codec pts      */

    /* Frames the codec may still hold, oldest first, by codec pts: the tag
     * each was submitted with, for its packet (md-f7uc). */
    struct { int64_t pts; uint64_t tag; } inflight[MD_ENCODER_INFLIGHT];
    uint32_t             inflight_head, inflight_count;

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

/* ── Per-frame tags across the codec ─────────────────────────── */

static void inflight_push(MdEncoder *enc, int64_t pts, uint64_t tag) {
    if (enc->inflight_count == MD_ENCODER_INFLIGHT) {
        enc->inflight_head = (enc->inflight_head + 1u) % MD_ENCODER_INFLIGHT;
        enc->inflight_count--;
    }
    const uint32_t i = (enc->inflight_head + enc->inflight_count) %
                       MD_ENCODER_INFLIGHT;
    enc->inflight[i].pts = pts;
    enc->inflight[i].tag = tag;
    enc->inflight_count++;
}

/* The tag of the frame with codec timestamp pts. Codec timestamps strictly
 * increase, so frames held before it were dropped by the codec and are
 * forgotten with it. */
static uint64_t inflight_take(MdEncoder *enc, int64_t pts) {
    for (uint32_t n = 0; n < enc->inflight_count; n++) {
        const uint32_t i = (enc->inflight_head + n) % MD_ENCODER_INFLIGHT;
        if (enc->inflight[i].pts > pts)
            break;
        if (enc->inflight[i].pts == pts) {
            enc->inflight_head = (i + 1u) % MD_ENCODER_INFLIGHT;
            enc->inflight_count -= n + 1u;
            return enc->inflight[i].tag;
        }
    }
    return 0;
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
                .tag    = inflight_take(enc, enc->pkt->pts),
            };
            enc->cb(&out, enc->cb_userdata);
        } else {
            (void)inflight_take(enc, enc->pkt->pts);
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
    ctx->gop_size  = 0;           /* existing hardware intra-refresh policy */
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
    } else if (strcmp(codec_name, "h264_mf") == 0) {
        ctx->gop_size = (int)fps * 2;
        /* With hw_encoding=0 FFmpeg enumerates the OS software MFT too. */
        av_dict_set(&opts, "hw_encoding", "0", 0);
        av_dict_set(&opts, "scenario", "display_remoting", 0);
        av_dict_set(&opts, "rate_control", "cbr", 0);
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    } else if (strcmp(codec_name, "h264_qsv") == 0) {
        ctx->gop_size = (int)fps * 2;
        av_dict_set(&opts, "preset", "veryfast", 0);
        av_dict_set(&opts, "async_depth", "1", 0);
        av_dict_set(&opts, "look_ahead", "0", 0);
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    } else if (strcmp(codec_name, "h264_vaapi") == 0) {
        ctx->gop_size = (int)fps * 2;
        AVBufferRef *device = NULL;
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) < 0) {
            avcodec_free_context(&ctx);
            return -1;
        }
        ctx->hw_frames_ctx = av_hwframe_ctx_alloc(device);
        av_buffer_unref(&device);
        if (!ctx->hw_frames_ctx) {
            avcodec_free_context(&ctx);
            return -1;
        }
        AVHWFramesContext *frames = (AVHWFramesContext *)ctx->hw_frames_ctx->data;
        frames->format = AV_PIX_FMT_VAAPI;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = (int)w;
        frames->height = (int)h;
        frames->initial_pool_size = 4;
        if (av_hwframe_ctx_init(ctx->hw_frames_ctx) < 0) {
            avcodec_free_context(&ctx);
            return -1;
        }
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;
        av_dict_set(&opts, "rc_mode", "CBR", 0);
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

static bool attempt_encoder(void *userdata, const char *name) {
    MdEncoder *enc = userdata;
    if (strcmp(name, "openh264") == 0) {
        enc->openh264 = md_openh264_create(&enc->config);
        return enc->openh264 != NULL;
    }
    if (try_open_encoder(enc, name) < 0) return false;
    /* MF may be the OS software MFT; don't label it as GPU hardware. */
    enc->is_hw = strcmp(name, "libx264") != 0 && strcmp(name, "h264_mf") != 0;
    return true;
}

/* ── Public API ──────────────────────────────────────────────── */

MdEncoder *md_encoder_create(const MdEncoderConfig *cfg) {
    if (!cfg || cfg->width == 0 || cfg->height == 0)
        return NULL;

    if (cfg->width > (uint32_t)INT_MAX || cfg->height > (uint32_t)INT_MAX ||
        cfg->fps > (uint32_t)INT_MAX / 2u ||
        cfg->bitrate > MD_ENCODER_MAX_BITRATE ||
        (uint64_t)cfg->width * cfg->height > (uint64_t)INT_MAX / 4u)
        return NULL;

    /* Width and height must be even for NV12 */
    if (cfg->width % 2 != 0 || cfg->height % 2 != 0)
        return NULL;

    MdEncoder *enc = calloc(1, sizeof(MdEncoder));
    if (!enc) return NULL;

    enc->config = *cfg;

    /* Copy optional strings: resize retains the config after the caller returns. */
    if (cfg->openh264_path) enc->openh264_path = strdup(cfg->openh264_path);
    if (cfg->encoder_name) enc->encoder_name = strdup(cfg->encoder_name);
    if ((cfg->openh264_path && !enc->openh264_path) ||
        (cfg->encoder_name && !enc->encoder_name)) {
        md_encoder_destroy(enc); return NULL;
    }
    enc->config.openh264_path = enc->openh264_path;
    enc->config.encoder_name = enc->encoder_name;
#if defined(_WIN32)
    MdEncoderPlatform platform = MD_ENCODER_WINDOWS;
#elif defined(__APPLE__)
    MdEncoderPlatform platform = MD_ENCODER_MACOS;
#else
    MdEncoderPlatform platform = MD_ENCODER_LINUX;
#endif
    if (!md_encoder_select(platform, cfg->prefer_nvenc, cfg->encoder_name,
                           attempt_encoder, enc)) {
        MD_LOG_E("No H.264 encoder available. Install Cisco OpenH264 2.7.0 and "
                 "set --openh264-path; see src/core/encoder-runtime.md");
        md_encoder_destroy(enc);
        return NULL;
    }

    /* CPU NV12 input, also used before VAAPI upload and OpenH264 I420 conversion. */
    enc->frame = av_frame_alloc();
    enc->pkt = av_packet_alloc();
    enc->hw_frame = av_frame_alloc();
    if (!enc->frame || !enc->pkt || !enc->hw_frame) {
        md_encoder_destroy(enc); return NULL;
    }
    enc->frame->format = AV_PIX_FMT_NV12;
    enc->frame->width = (int)cfg->width;
    enc->frame->height = (int)cfg->height;
    if (av_frame_get_buffer(enc->frame, 32) < 0) {
        md_encoder_destroy(enc); return NULL;
    }

    return enc;
}

int md_encoder_submit(MdEncoder *enc, const uint8_t *data,
                      uint32_t stride, MdPixFmt input_fmt,
                      int64_t pts,
                      MdEncodeCallback cb, void *userdata) {
    return md_encoder_submit_tagged(enc, data, stride, input_fmt, pts, 0,
                                    cb, userdata);
}

int md_encoder_submit_tagged(MdEncoder *enc, const uint8_t *data,
                             uint32_t stride, MdPixFmt input_fmt,
                             int64_t pts, uint64_t tag,
                             MdEncodeCallback cb, void *userdata) {
    if (!enc || !data || (!enc->ctx && !enc->openh264))
        return -1;

    if (av_frame_make_writable(enc->frame) < 0) return -1;
    /* Convert input pixels to NV12 via libyuv */
    int ret = convert_to_nv12(data, stride, input_fmt,
                              enc->config.width,
                              enc->config.height,
                              enc->frame->data[0], enc->frame->linesize[0],
                              enc->frame->data[1], enc->frame->linesize[1]);
    if (ret < 0) {
        enc->cb = NULL;
        enc->cb_userdata = NULL;
        return -1;
    }

    /* Set PTS. FFmpeg expects PTS in the codec's time_base; use the
     * caller's timestamp when provided, else fall back to frame count.
     * Strictly increasing, so each held frame's packet finds its tag (a
     * capture restart can restart the caller's count). */
    int64_t codec_pts = (pts >= 0) ? pts : enc->frame_idx;
    if (codec_pts < enc->frame_idx)
        codec_pts = enc->frame_idx;
    if (codec_pts == INT64_MAX ||
        (enc->openh264 && codec_pts > INT64_MAX / 1000)) return -1;
    enc->cb = cb;
    enc->cb_userdata = userdata;
    enc->frame->pts = codec_pts;
    enc->frame_idx = codec_pts + 1;

    if (enc->openh264) {
        MdEncodedPacket out;
        bool force = enc->force_keyframe;
        enc->force_keyframe = false;
        ret = md_openh264_encode(enc->openh264, enc->frame->data[0], enc->frame->linesize[0],
                                 enc->frame->data[1], enc->frame->linesize[1], codec_pts, force, &out);
        if (ret == 0) {
            inflight_push(enc, codec_pts, tag);
            if (out.size) {
                out.tag = inflight_take(enc, out.pts);
                if (cb) cb(&out, userdata);
            }
        }
        enc->cb = NULL; enc->cb_userdata = NULL;
        return ret;
    }
    enc->frame->pict_type = enc->force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    enc->force_keyframe = false;
    AVFrame *input = enc->frame;
    if (enc->ctx->pix_fmt == AV_PIX_FMT_VAAPI) {
        av_frame_unref(enc->hw_frame);
        if (av_hwframe_get_buffer(enc->ctx->hw_frames_ctx, enc->hw_frame, 0) < 0 ||
            av_hwframe_transfer_data(enc->hw_frame, enc->frame, 0) < 0 ||
            av_frame_copy_props(enc->hw_frame, enc->frame) < 0) {
            enc->cb = NULL; enc->cb_userdata = NULL;
            return -1;
        }
        input = enc->hw_frame;
    }
    ret = avcodec_send_frame(enc->ctx, input);
    if (ret == AVERROR(EAGAIN)) {
        if (receive_packets(enc) < 0) {
            enc->cb = NULL; enc->cb_userdata = NULL;
            return -1;
        }
        ret = avcodec_send_frame(enc->ctx, input);
    }
    if (ret < 0) {
        enc->cb = NULL; enc->cb_userdata = NULL;
        return -1;
    }
    inflight_push(enc, codec_pts, tag);

    /* Receive any available encoded packets */
    ret = receive_packets(enc);

    enc->cb = NULL;
    enc->cb_userdata = NULL;

    if (ret < 0)
        return -1;

    return 0;
}

int md_encoder_flush(MdEncoder *enc, MdEncodeCallback cb, void *userdata) {
    if (!enc) return -1;
    if (enc->openh264) return 0; /* EncodeFrame has no delayed output. */
    if (!enc->ctx) return -1;

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

/* Open a complete encoder at the new size through the normal cascade, then
 * swap it into enc so the handle the caller holds stays valid. Building the
 * replacement first leaves enc untouched when the new size cannot open. */
int md_encoder_resize(MdEncoder *enc, uint32_t width, uint32_t height) {
    if (!enc)
        return -1;
    if (enc->config.width == width && enc->config.height == height)
        return 0;

    MdEncoderConfig cfg = enc->config; /* carries the current bitrate */
    cfg.width  = width;
    cfg.height = height;
    MdEncoder *fresh = md_encoder_create(&cfg);
    if (!fresh)
        return -1;

    fresh->frame_idx = enc->frame_idx;
    fresh->force_keyframe = true;
    MdEncoder old = *enc;
    *enc = *fresh;
    *fresh = old;
    md_encoder_destroy(fresh); /* frees the old codec state, unflushed */
    return 0;
}

/* Request that the next submitted frame be encoded as a keyframe (IDR), so a
 * newly connected client has an immediate decodable entry point. */
void md_encoder_force_keyframe(MdEncoder *enc) {
    if (enc)
        enc->force_keyframe = true;
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
    if (!enc) return -1;
    uint32_t br = clamp_bitrate(new_bitrate);
    if (enc->openh264) {
        if (md_openh264_bitrate(enc->openh264, br) < 0) return -1;
        enc->config.bitrate = br;
        return 0;
    }
    if (!enc->ctx) return -1;

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

const char *md_encoder_get_name(const MdEncoder *enc) {
    return enc && enc->openh264 ? "openh264" : enc && enc->codec ? enc->codec->name : NULL;
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
    if (enc->frame)
        av_frame_free(&enc->frame);
    if (enc->ctx)
        avcodec_free_context(&enc->ctx);

    md_openh264_destroy(enc->openh264);
    av_frame_free(&enc->hw_frame);
    free(enc->openh264_path);
    free(enc->encoder_name);
    free(enc);
}
