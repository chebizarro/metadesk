/*
 * metadesk — encode_videotoolbox.m
 * Native VideoToolbox H.264 encoder (spec §9.2).
 *
 * Provides md_vt_encoder_* API for direct VTCompressionSession usage
 * without requiring FFmpeg. Used as a standalone encoder or as a
 * fallback when FFmpeg is not available.
 *
 * VTCompressionSession flow:
 *   1. VTCompressionSessionCreate — configure codec, dimensions, properties
 *   2. VTCompressionSessionEncodeFrame — submit CVPixelBuffer
 *   3. Output callback — receives CMSampleBuffer with H.264 NAL units
 *   4. Extract NAL data from CMBlockBuffer
 *
 * Requires macOS 10.8+ (VideoToolbox framework).
 * Built as Objective-C (.m) for @autoreleasepool.
 */
#include "encode.h"

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* libyuv for colorspace conversion (BGRA/RGBA → NV12) */
#include <libyuv.h>

/* ── Native VT encoder context ───────────────────────────────── */

typedef struct MdVTEncoder {
    uint32_t                width;
    uint32_t                height;
    uint32_t                fps;
    uint32_t                bitrate;
    VTCompressionSessionRef session;
    CVPixelBufferPoolRef    pixbuf_pool;
    int64_t                 frame_idx;

    /* Callback state (set during submit, used in output callback) */
    MdEncodeCallback        cb;
    void                   *cb_userdata;

    /* Scratch buffer for NAL extraction */
    uint8_t                *nal_buf;
    size_t                  nal_buf_cap;
} MdVTEncoder;

/* ── VT output callback ─────────────────────────────────────── */

static void vt_output_callback(void *outputCallbackRefCon,
                                void *sourceFrameRefCon,
                                OSStatus status,
                                VTEncodeInfoFlags infoFlags,
                                CMSampleBufferRef sampleBuffer) {
    (void)sourceFrameRefCon;
    (void)infoFlags;

    MdVTEncoder *enc = (MdVTEncoder *)outputCallbackRefCon;
    if (status != noErr || !sampleBuffer || !enc->cb)
        return;

    /* Check if keyframe */
    bool is_key = false;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef notSync = NULL;
        if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync,
                                           (const void **)&notSync)) {
            is_key = !CFBooleanGetValue(notSync);
        } else {
            is_key = true; /* absence of NotSync means it IS a sync sample */
        }
    }

    /* Get timestamps */
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    CMTime dts = CMSampleBufferGetDecodeTimeStamp(sampleBuffer);
    int64_t pts_val = CMTIME_IS_VALID(pts) ? (int64_t)(CMTimeGetSeconds(pts) * 1e6) : 0;
    int64_t dts_val = CMTIME_IS_VALID(dts) ? (int64_t)(CMTimeGetSeconds(dts) * 1e6) : pts_val;

    /* Extract NAL units from CMBlockBuffer.
     * VideoToolbox outputs in AVCC format (length-prefixed NALs).
     * We convert to Annex B (start code prefixed) for network transport. */
    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!blockBuffer) return;

    size_t totalLength = 0;
    char *dataPointer = NULL;
    OSStatus err = CMBlockBufferGetDataPointer(blockBuffer, 0, NULL,
                                                &totalLength, &dataPointer);
    if (err != kCMBlockBufferNoErr || !dataPointer || totalLength == 0)
        return;

    /* For keyframes, prepend SPS/PPS from the format description */
    CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
    size_t paramOffset = 0;

    if (is_key && formatDesc) {
        /* Get SPS */
        const uint8_t *sps = NULL;
        size_t spsSize = 0;
        size_t spsCount = 0;
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 0,
            &sps, &spsSize, &spsCount, NULL);

        /* Get PPS */
        const uint8_t *pps = NULL;
        size_t ppsSize = 0;
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 1,
            &pps, &ppsSize, NULL, NULL);

        /* Calculate total size: start codes + SPS + PPS + NAL data */
        size_t needed = 4 + spsSize + 4 + ppsSize + totalLength + 256;
        if (needed > enc->nal_buf_cap) {
            enc->nal_buf_cap = needed;
            enc->nal_buf = realloc(enc->nal_buf, enc->nal_buf_cap);
            if (!enc->nal_buf) { enc->nal_buf_cap = 0; return; }
        }

        /* Write SPS with Annex B start code */
        if (sps && spsSize > 0) {
            enc->nal_buf[paramOffset++] = 0x00;
            enc->nal_buf[paramOffset++] = 0x00;
            enc->nal_buf[paramOffset++] = 0x00;
            enc->nal_buf[paramOffset++] = 0x01;
            memcpy(enc->nal_buf + paramOffset, sps, spsSize);
            paramOffset += spsSize;
        }

        /* Write PPS with Annex B start code */
        if (pps && ppsSize > 0) {
            enc->nal_buf[paramOffset++] = 0x00;
            enc->nal_buf[paramOffset++] = 0x00;
            enc->nal_buf[paramOffset++] = 0x00;
            enc->nal_buf[paramOffset++] = 0x01;
            memcpy(enc->nal_buf + paramOffset, pps, ppsSize);
            paramOffset += ppsSize;
        }
    } else {
        /* Non-keyframe: just need space for NAL data with start codes */
        size_t needed = totalLength + 256;
        if (needed > enc->nal_buf_cap) {
            enc->nal_buf_cap = needed;
            enc->nal_buf = realloc(enc->nal_buf, enc->nal_buf_cap);
            if (!enc->nal_buf) { enc->nal_buf_cap = 0; return; }
        }
    }

    /* Convert AVCC (length-prefixed) NALs to Annex B (start-code-prefixed) */
    size_t offset = 0;
    size_t outOffset = paramOffset;

    /* Get NAL length header size (usually 4 bytes) */
    int nalLengthHeaderSize = 4;
    if (formatDesc) {
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 0,
            NULL, NULL, NULL, &nalLengthHeaderSize);
    }

    while (offset < totalLength) {
        /* Read NAL length */
        uint32_t nalLen = 0;
        if (nalLengthHeaderSize == 4) {
            nalLen = ((uint32_t)(uint8_t)dataPointer[offset] << 24) |
                     ((uint32_t)(uint8_t)dataPointer[offset + 1] << 16) |
                     ((uint32_t)(uint8_t)dataPointer[offset + 2] << 8) |
                     ((uint32_t)(uint8_t)dataPointer[offset + 3]);
        } else if (nalLengthHeaderSize == 2) {
            nalLen = ((uint32_t)(uint8_t)dataPointer[offset] << 8) |
                     ((uint32_t)(uint8_t)dataPointer[offset + 1]);
        } else {
            break; /* unsupported */
        }
        offset += (size_t)nalLengthHeaderSize;

        if (nalLen == 0 || offset + nalLen > totalLength)
            break;

        /* Write Annex B start code + NAL data */
        enc->nal_buf[outOffset++] = 0x00;
        enc->nal_buf[outOffset++] = 0x00;
        enc->nal_buf[outOffset++] = 0x00;
        enc->nal_buf[outOffset++] = 0x01;
        memcpy(enc->nal_buf + outOffset, dataPointer + offset, nalLen);
        outOffset += nalLen;
        offset += nalLen;
    }

    /* Deliver to callback */
    MdEncodedPacket pkt = {
        .data   = enc->nal_buf,
        .size   = outOffset,
        .pts    = pts_val,
        .dts    = dts_val,
        .is_key = is_key,
    };
    enc->cb(&pkt, enc->cb_userdata);
}

/* ── Native VT encoder API ───────────────────────────────────── */

MdVTEncoder *md_vt_encoder_create(const MdEncoderConfig *cfg) {
    if (!cfg || cfg->width == 0 || cfg->height == 0)
        return NULL;

    MdVTEncoder *enc = calloc(1, sizeof(MdVTEncoder));
    if (!enc) return NULL;

    enc->width   = cfg->width;
    enc->height  = cfg->height;
    enc->fps     = cfg->fps ? cfg->fps : MD_ENCODER_DEFAULT_FPS;
    enc->bitrate = cfg->bitrate ? cfg->bitrate : MD_ENCODER_DEFAULT_BITRATE;

    /* Create VTCompressionSession */
    CFMutableDictionaryRef encoderSpec = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    /* Require hardware encoder (spec §9.2: allow_sw = false) */
    CFDictionarySetValue(encoderSpec,
        kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
        kCFBooleanTrue);

    /* Source pixel buffer attributes */
    CFMutableDictionaryRef pixBufAttrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    int32_t pixFmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange; /* NV12 */
    CFNumberRef pixFmtNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixFmt);
    CFDictionarySetValue(pixBufAttrs, kCVPixelBufferPixelFormatTypeKey, pixFmtNum);
    CFRelease(pixFmtNum);

    int32_t iWidth = (int32_t)enc->width;
    int32_t iHeight = (int32_t)enc->height;
    CFNumberRef widthNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &iWidth);
    CFNumberRef heightNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &iHeight);
    CFDictionarySetValue(pixBufAttrs, kCVPixelBufferWidthKey, widthNum);
    CFDictionarySetValue(pixBufAttrs, kCVPixelBufferHeightKey, heightNum);
    CFRelease(widthNum);
    CFRelease(heightNum);

    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        (int32_t)enc->width,
        (int32_t)enc->height,
        kCMVideoCodecType_H264,
        encoderSpec,
        pixBufAttrs,
        kCFAllocatorDefault,
        vt_output_callback,
        enc,
        &enc->session);

    CFRelease(encoderSpec);
    CFRelease(pixBufAttrs);

    if (status != noErr) {
        fprintf(stderr, "encode_vt: VTCompressionSessionCreate failed: %d\n", (int)status);
        free(enc);
        return NULL;
    }

    /* Configure session properties (spec §9.2) */
    VTSessionSetProperty(enc->session,
        kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    VTSessionSetProperty(enc->session,
        kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    int32_t maxBFrames = 0;
    CFNumberRef maxBNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &maxBFrames);
    VTSessionSetProperty(enc->session,
        kVTCompressionPropertyKey_MaxKeyFrameInterval, maxBNum);
    CFRelease(maxBNum);

    /* Bitrate */
    int32_t br = (int32_t)enc->bitrate;
    CFNumberRef brNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &br);
    VTSessionSetProperty(enc->session,
        kVTCompressionPropertyKey_AverageBitRate, brNum);
    CFRelease(brNum);

    /* Profile: High */
    VTSessionSetProperty(enc->session,
        kVTCompressionPropertyKey_ProfileLevel,
        kVTProfileLevel_H264_High_AutoLevel);

    /* Expected frame rate */
    int32_t iFps = (int32_t)enc->fps;
    CFNumberRef fpsNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &iFps);
    VTSessionSetProperty(enc->session,
        kVTCompressionPropertyKey_ExpectedFrameRate, fpsNum);
    CFRelease(fpsNum);

    /* Prepare to encode */
    status = VTCompressionSessionPrepareToEncodeFrames(enc->session);
    if (status != noErr) {
        fprintf(stderr, "encode_vt: PrepareToEncodeFrames failed: %d\n", (int)status);
        VTCompressionSessionInvalidate(enc->session);
        CFRelease(enc->session);
        free(enc);
        return NULL;
    }

    /* Get pixel buffer pool from session */
    enc->pixbuf_pool = VTCompressionSessionGetPixelBufferPool(enc->session);

    /* Allocate NAL scratch buffer */
    enc->nal_buf_cap = 256 * 1024; /* 256 KB initial */
    enc->nal_buf = malloc(enc->nal_buf_cap);

    return enc;
}

int md_vt_encoder_submit(MdVTEncoder *enc, const uint8_t *data,
                         uint32_t stride, MdPixFmt input_fmt,
                         int64_t pts,
                         MdEncodeCallback cb, void *userdata) {
    (void)pts; /* PTS derived from frame_idx for monotonic timebase */

    if (!enc || !data || !enc->session)
        return -1;

    enc->cb = cb;
    enc->cb_userdata = userdata;

    /* Create a CVPixelBuffer for the frame */
    CVPixelBufferRef pixelBuffer = NULL;
    CVReturn cvRet;

    if (enc->pixbuf_pool) {
        cvRet = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
                                                    enc->pixbuf_pool,
                                                    &pixelBuffer);
    } else {
        cvRet = CVPixelBufferCreate(kCFAllocatorDefault,
                                     enc->width, enc->height,
                                     kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                     NULL, &pixelBuffer);
    }

    if (cvRet != kCVReturnSuccess || !pixelBuffer)
        return -1;

    /* Lock and fill with NV12 data (converted from input format via libyuv) */
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);

    uint8_t *dst_y  = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
    uint8_t *dst_uv = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
    int dst_stride_y  = (int)CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
    int dst_stride_uv = (int)CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

    int convRet = -1;
    switch (input_fmt) {
    case MD_PIX_FMT_BGRX:
    case MD_PIX_FMT_BGRA:
        convRet = ARGBToNV12(data, (int)stride,
                             dst_y, dst_stride_y,
                             dst_uv, dst_stride_uv,
                             (int)enc->width, (int)enc->height);
        break;
    case MD_PIX_FMT_RGBX:
    case MD_PIX_FMT_RGBA:
        convRet = ABGRToNV12(data, (int)stride,
                             dst_y, dst_stride_y,
                             dst_uv, dst_stride_uv,
                             (int)enc->width, (int)enc->height);
        break;
    case MD_PIX_FMT_NV12:
        {
            int y_size = dst_stride_y * (int)enc->height;
            int uv_size = dst_stride_uv * ((int)enc->height / 2);
            memcpy(dst_y, data, (size_t)y_size);
            memcpy(dst_uv, data + (stride * enc->height), (size_t)uv_size);
        }
        convRet = 0;
        break;
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    if (convRet != 0) {
        CVPixelBufferRelease(pixelBuffer);
        return -1;
    }

    /* Submit to VTCompressionSession */
    CMTime cmPts = CMTimeMake(enc->frame_idx++, (int32_t)enc->fps);
    CMTime duration = CMTimeMake(1, (int32_t)enc->fps);

    OSStatus status = VTCompressionSessionEncodeFrame(
        enc->session, pixelBuffer, cmPts, duration,
        NULL, NULL, NULL);

    CVPixelBufferRelease(pixelBuffer);

    if (status != noErr)
        return -1;

    /* The callback fires synchronously or asynchronously depending on
     * the session configuration. Force synchronous completion: */
    VTCompressionSessionCompleteFrames(enc->session, cmPts);

    enc->cb = NULL;
    enc->cb_userdata = NULL;
    return 0;
}

int md_vt_encoder_flush(MdVTEncoder *enc, MdEncodeCallback cb, void *userdata) {
    if (!enc || !enc->session)
        return -1;

    enc->cb = cb;
    enc->cb_userdata = userdata;

    OSStatus status = VTCompressionSessionCompleteFrames(enc->session,
        kCMTimeInvalid);

    enc->cb = NULL;
    enc->cb_userdata = NULL;

    return (status == noErr) ? 0 : -1;
}

bool md_vt_encoder_is_hw(const MdVTEncoder *enc) {
    (void)enc;
    return true; /* VT is always hardware on Apple Silicon / Intel with GPU */
}

int md_vt_encoder_get_size(const MdVTEncoder *enc, uint32_t *width, uint32_t *height) {
    if (!enc || !width || !height) return -1;
    *width = enc->width;
    *height = enc->height;
    return 0;
}

void md_vt_encoder_destroy(MdVTEncoder *enc) {
    if (!enc) return;

    if (enc->session) {
        VTCompressionSessionCompleteFrames(enc->session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(enc->session);
        CFRelease(enc->session);
    }

    free(enc->nal_buf);
    free(enc);
}
