/*
 * metadesk — capture_screencapturekit.m
 * macOS capture backend: ScreenCaptureKit (spec §2.3.1).
 *
 * Flow:
 *   1. SCShareableContent.getExcludingDesktopWindows — enumerate displays
 *   2. SCStreamConfiguration — set resolution, framerate, pixel format
 *   3. SCStream — create stream for main display
 *   4. addStreamOutput — receive CMSampleBuffers via delegate
 *   5. CVPixelBuffer lock/unlock — map frame data for consumer
 *
 * ScreenCaptureKit delivers frames on an internal dispatch queue.
 * Frames are handed to the consumer via get_frame / release_frame
 * synchronised with a mutex + condition variable (same pattern as
 * the PipeWire backend).
 *
 * Requires macOS 12.3+ (ScreenCaptureKit).
 * Built as Objective-C (.m) — see meson.build darwin block.
 */
#include "capture.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
    /* ScreenCaptureKit objects (bridged to void* for C struct) */
    void *stream;          /* SCStream *           */
    void *delegate;        /* MdSCKDelegate *      */
    void *captureQueue;    /* dispatch_queue_t     */

    /* Frame delivery (SCK queue → consumer) */
    pthread_mutex_t         frame_lock;
    pthread_cond_t          frame_cond;
    MdFrame                 pending_frame;
    bool                    frame_ready;

    /* Currently held sample buffer (retained until release_frame) */
    void                   *held_sample_buf;   /* CMSampleBufferRef */
    void                   *held_pixel_buf;    /* CVPixelBufferRef (locked) */

    /* Sequence counter */
    atomic_uint_least32_t   seq;

    /* Back-pointer */
    MdCaptureCtx           *ctx;
} SCKState;

/* ── SCStreamOutput delegate ─────────────────────────────────── */

API_AVAILABLE(macos(12.3))
@interface MdSCKDelegate : NSObject <SCStreamOutput>
@property (assign) SCKState *state;
@end

@implementation MdSCKDelegate

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
               ofType:(SCStreamOutputType)type
    API_AVAILABLE(macos(12.3))
{
    if (type != SCStreamOutputTypeScreen)
        return;

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer)
        return;

    /* Lock the pixel buffer base address for CPU access */
    CVReturn lockResult = CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
    if (lockResult != kCVReturnSuccess)
        return;

    SCKState *sck = self.state;
    MdCaptureCtx *ctx = sck->ctx;

    uint32_t w = (uint32_t)CVPixelBufferGetWidth(imageBuffer);
    uint32_t h = (uint32_t)CVPixelBufferGetHeight(imageBuffer);
    size_t stride = CVPixelBufferGetBytesPerRow(imageBuffer);
    uint8_t *base = CVPixelBufferGetBaseAddress(imageBuffer);
    size_t dataSize = CVPixelBufferGetDataSize(imageBuffer);

    /* Map CVPixelBuffer format → MdCapturePixFmt */
    OSType pixFmt = CVPixelBufferGetPixelFormatType(imageBuffer);
    MdCapturePixFmt capFmt;
    switch (pixFmt) {
    case kCVPixelFormatType_32BGRA:
        capFmt = MD_PIX_CAPTURE_BGRA;
        break;
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        capFmt = MD_PIX_CAPTURE_NV12;
        break;
    case kCVPixelFormatType_32RGBA:
        capFmt = MD_PIX_CAPTURE_RGBA;
        break;
    default:
        capFmt = MD_PIX_CAPTURE_BGRA; /* best guess */
        break;
    }

    /* Get timestamp */
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    uint64_t ts_ns = 0;
    if (CMTIME_IS_VALID(pts)) {
        double seconds = CMTimeGetSeconds(pts);
        ts_ns = (uint64_t)(seconds * 1e9);
    }

    MdFrame frame = {
        .width        = w,
        .height       = h,
        .stride       = (uint32_t)stride,
        .format       = capFmt,
        .buf_type     = MD_BUF_CPU,
        .dmabuf_fd    = -1,
        .data         = base,
        .gpu_handle   = NULL,
        .data_size    = dataSize,
        .timestamp_ns = ts_ns,
        .seq          = atomic_fetch_add_explicit(&sck->seq, 1, memory_order_relaxed),
    };

    /* Update context dimensions */
    ctx->width  = w;
    ctx->height = h;

    pthread_mutex_lock(&sck->frame_lock);

    /* If a previous frame is pending (consumer too slow), release it */
    if (sck->frame_ready && sck->held_pixel_buf) {
        CVPixelBufferUnlockBaseAddress((CVPixelBufferRef)sck->held_pixel_buf,
                                       kCVPixelBufferLock_ReadOnly);
        CFRelease(sck->held_sample_buf);
        sck->held_sample_buf = NULL;
        sck->held_pixel_buf  = NULL;
    }

    /* Retain the sample buffer so the pixel data stays valid */
    CFRetain(sampleBuffer);
    sck->held_sample_buf = (void *)sampleBuffer;
    sck->held_pixel_buf  = (void *)imageBuffer;
    sck->pending_frame   = frame;
    sck->frame_ready     = true;

    pthread_cond_signal(&sck->frame_cond);
    pthread_mutex_unlock(&sck->frame_lock);
}

@end

/* ── Backend vtable implementation ───────────────────────────── */

static int sck_init(MdCaptureCtx *ctx, const MdCaptureConfig *cfg) {
    (void)cfg;

    SCKState *sck = calloc(1, sizeof(SCKState));
    if (!sck) return -1;

    sck->ctx = ctx;
    pthread_mutex_init(&sck->frame_lock, NULL);
    pthread_cond_init(&sck->frame_cond, NULL);

    ctx->backend_data = sck;
    return 0;
}

static int sck_start(MdCaptureCtx *ctx) {
    if (@available(macOS 12.3, *)) {
        /* ok */
    } else {
        fprintf(stderr, "capture: ScreenCaptureKit requires macOS 12.3+\n");
        return -1;
    }

    SCKState *sck = ctx->backend_data;
    if (!sck) return -1;

    /* Enumerate shareable content (synchronous via semaphore) */
    __block SCShareableContent *shareableContent = nil;
    __block NSError *contentError = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentExcludingDesktopWindows:YES
                                              onScreenWindowsOnly:NO
                                                completionHandler:^(SCShareableContent * _Nullable content,
                                                                    NSError * _Nullable error) {
        shareableContent = content;
        contentError = error;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    if (!shareableContent || contentError) {
        fprintf(stderr, "capture: failed to get shareable content: %s\n",
                contentError ? [[contentError localizedDescription] UTF8String] : "unknown error");
        return -1;
    }

    /* Pick the main display (first in list) */
    NSArray<SCDisplay *> *displays = shareableContent.displays;
    if (displays.count == 0) {
        fprintf(stderr, "capture: no displays found\n");
        return -1;
    }
    SCDisplay *mainDisplay = displays[0];

    /* Configure the stream */
    SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
    config.width = mainDisplay.width;
    config.height = mainDisplay.height;
    config.showsCursor = ctx->config.show_cursor;

    /* Request BGRA pixel format (most compatible with our encoder) */
    config.pixelFormat = kCVPixelFormatType_32BGRA;

    /* Frame rate */
    uint32_t fps = ctx->config.target_fps ? ctx->config.target_fps : 60;
    config.minimumFrameInterval = CMTimeMake(1, (int32_t)fps);

    /* Create content filter for the main display (all windows) */
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:mainDisplay
                                                      excludingWindows:@[]];

    /* Create the stream */
    SCStream *stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:config
                                               delegate:nil];

    /* Create delegate and dispatch queue */
    MdSCKDelegate *delegate = [[MdSCKDelegate alloc] init];
    delegate.state = sck;

    dispatch_queue_t captureQueue = dispatch_queue_create("com.metadesk.capture.sck",
                                                          DISPATCH_QUEUE_SERIAL);

    NSError *addOutputError = nil;
    [stream addStreamOutput:delegate
                       type:SCStreamOutputTypeScreen
             sampleHandlerQueue:captureQueue
                      error:&addOutputError];
    if (addOutputError) {
        fprintf(stderr, "capture: failed to add stream output: %s\n",
                [[addOutputError localizedDescription] UTF8String]);
        return -1;
    }

    /* Store references */
    sck->stream       = (__bridge_retained void *)stream;
    sck->delegate     = (__bridge_retained void *)delegate;
    sck->captureQueue = (__bridge_retained void *)captureQueue;

    /* Start capture (async, wait for completion) */
    __block NSError *startError = nil;
    dispatch_semaphore_t startSem = dispatch_semaphore_create(0);

    [stream startCaptureWithCompletionHandler:^(NSError * _Nullable error) {
        startError = error;
        dispatch_semaphore_signal(startSem);
    }];
    dispatch_semaphore_wait(startSem, DISPATCH_TIME_FOREVER);

    if (startError) {
        fprintf(stderr, "capture: failed to start capture: %s\n",
                [[startError localizedDescription] UTF8String]);
        return -1;
    }

    ctx->active = true;
    ctx->width  = mainDisplay.width;
    ctx->height = mainDisplay.height;

    return 0;
}

static int sck_get_frame(MdCaptureCtx *ctx, MdFrame *out) {
    SCKState *sck = ctx->backend_data;
    if (!sck) return -1;

    pthread_mutex_lock(&sck->frame_lock);

    /* Wait for a frame from the SCK delegate */
    while (!sck->frame_ready && ctx->active) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000L; /* 100ms timeout */
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec += 1;
        }
        pthread_cond_timedwait(&sck->frame_cond, &sck->frame_lock, &ts);
    }

    if (!sck->frame_ready) {
        pthread_mutex_unlock(&sck->frame_lock);
        return -1; /* capture stopped */
    }

    *out = sck->pending_frame;
    sck->frame_ready = false;
    pthread_mutex_unlock(&sck->frame_lock);
    return 0;
}

static void sck_release_frame(MdCaptureCtx *ctx, MdFrame *frame) {
    SCKState *sck = ctx->backend_data;
    (void)frame;
    if (!sck) return;

    pthread_mutex_lock(&sck->frame_lock);
    if (sck->held_pixel_buf) {
        CVPixelBufferUnlockBaseAddress((CVPixelBufferRef)sck->held_pixel_buf,
                                       kCVPixelBufferLock_ReadOnly);
        sck->held_pixel_buf = NULL;
    }
    if (sck->held_sample_buf) {
        CFRelease(sck->held_sample_buf);
        sck->held_sample_buf = NULL;
    }
    pthread_mutex_unlock(&sck->frame_lock);
}

static void sck_stop(MdCaptureCtx *ctx) {
    if (@available(macOS 12.3, *)) {
        /* ok */
    } else {
        return;
    }

    SCKState *sck = ctx->backend_data;
    if (!sck) return;

    ctx->active = false;

    /* Wake any blocked get_frame */
    pthread_mutex_lock(&sck->frame_lock);
    pthread_cond_signal(&sck->frame_cond);
    pthread_mutex_unlock(&sck->frame_lock);

    if (sck->stream) {
        SCStream *stream = (__bridge SCStream *)sck->stream;
        dispatch_semaphore_t stopSem = dispatch_semaphore_create(0);
        [stream stopCaptureWithCompletionHandler:^(NSError * _Nullable error) {
            (void)error;
            dispatch_semaphore_signal(stopSem);
        }];
        dispatch_semaphore_wait(stopSem, DISPATCH_TIME_FOREVER);
    }
}

static void sck_destroy(MdCaptureCtx *ctx) {
    SCKState *sck = ctx->backend_data;
    if (!sck) return;

    /* Release any held frame */
    if (sck->held_pixel_buf) {
        CVPixelBufferUnlockBaseAddress((CVPixelBufferRef)sck->held_pixel_buf,
                                       kCVPixelBufferLock_ReadOnly);
    }
    if (sck->held_sample_buf) {
        CFRelease(sck->held_sample_buf);
    }

    /* Release ObjC objects */
    if (sck->stream)       { CFRelease(sck->stream); }
    if (sck->delegate)     { CFRelease(sck->delegate); }
    if (sck->captureQueue) { CFRelease(sck->captureQueue); }

    pthread_mutex_destroy(&sck->frame_lock);
    pthread_cond_destroy(&sck->frame_cond);

    free(sck);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdCaptureBackend screencapturekit_backend = {
    .init          = sck_init,
    .start         = sck_start,
    .get_frame     = sck_get_frame,
    .release_frame = sck_release_frame,
    .stop          = sck_stop,
    .destroy       = sck_destroy,
};

const MdCaptureBackend *md_capture_backend_create(void) {
    if (@available(macOS 12.3, *)) {
        return &screencapturekit_backend;
    }
    fprintf(stderr, "capture: ScreenCaptureKit requires macOS 12.3+\n");
    return NULL;
}
