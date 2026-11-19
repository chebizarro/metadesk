/*
 * metadesk — capture_screencapturekit.m
 * macOS capture backend: ScreenCaptureKit (spec §2.3.1).
 *
 * Flow:
 *   1. SCShareableContent.getExcludingDesktopWindows — enumerate displays
 *   2. SCStreamConfiguration — resolution (the display's backing pixels:
 *      points x pointPixelScale), framerate, pixel format
 *   3. SCStream — create stream for the selected display (config.output:
 *      NULL/"" = main display, index into shareableContent.displays,
 *      "id:N" CGDirectDisplayID, or unique NSScreen name; see capture.h)
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
#import <AppKit/AppKit.h>  /* NSScreen.localizedName for name selection */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "log.h"
#include "md_time.h"
#define MD_LOG_TAG "capture_sck"

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

    /* True while the consumer holds a frame handed out by get_frame.
     * The producer must not overwrite held_* in that state — the old
     * buffer would leak and the consumer's release_frame would release
     * the new one (the PipeWire backend already guards this). */
    bool                    frame_checked_out;

    /* One diagnostic for a pixel format outside the requested set. */
    bool                    format_warned;

    /* Sequence counter */
    atomic_uint_least32_t   seq;

    /* Last timestamp_ns handed out (capture queue only). */
    uint64_t                last_ts;

    /* Back-pointer */
    MdCaptureCtx           *ctx;
} SCKState;

/* ── Frame timestamps ──────────────────────────────────────── */

/* SCK stamps a frame with its display time on the CoreMedia host clock
 * (mach_absolute_time, which stops during sleep), at times a few ms ahead of
 * delivery. CLOCK_MONOTONIC keeps counting through sleep, so the gap between
 * the clocks grows with every sleep: rebase against both clocks read now
 * instead of caching an offset. */
static uint64_t sck_frame_clock_ns(CMTime pts) {
    const CMTime host_now = CMClockGetTime(CMClockGetHostTimeClock());
    const uint64_t now = md_capture_clock_ns();
    if (!CMTIME_IS_NUMERIC(pts) || !CMTIME_IS_NUMERIC(host_now)) return now;
    const CMTime p = CMTimeConvertScale(pts, 1000000000, kCMTimeRoundingMethod_Default);
    const CMTime h = CMTimeConvertScale(host_now, 1000000000, kCMTimeRoundingMethod_Default);
    if (!CMTIME_IS_NUMERIC(p) || !CMTIME_IS_NUMERIC(h) || p.value < 0 || h.value < 0)
        return now;
    return md_capture_rebase_ns((uint64_t)p.value, (uint64_t)h.value, now);
}

/* ── SCStreamOutput delegate ───────────────────────────────── */

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
        /* sck_start requests 32BGRA; never relabel unknown bytes. */
        if (!sck->format_warned) {
            sck->format_warned = true;
            MD_LOG_W("dropping frames with unsupported pixel format 0x%08x", (unsigned)pixFmt);
        }
        CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
        return;
    }

    uint64_t ts_ns = sck_frame_clock_ns(CMSampleBufferGetPresentationTimeStamp(sampleBuffer));
    if (ts_ns < sck->last_ts) ts_ns = sck->last_ts;
    sck->last_ts = ts_ns;

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

    if (sck->frame_checked_out) {
        /* Consumer still holds the previous frame — drop this one
         * instead of corrupting the buffer it is reading. */
        pthread_mutex_unlock(&sck->frame_lock);
        CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
        return;
    }

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

static int sck_init(MdCaptureCtx *ctx) {
    SCKState *sck = calloc(1, sizeof(SCKState));
    if (!sck) return -1;

    sck->ctx = ctx;
    pthread_mutex_init(&sck->frame_lock, NULL);
    md_cond_init_monotonic(&sck->frame_cond);

    ctx->backend_data = sck;
    return 0;
}

/* Name of each display by CGDirectDisplayID (NSScreen.localizedName). */
static NSDictionary<NSNumber *, NSString *> *sck_display_names(void) {
    NSMutableDictionary<NSNumber *, NSString *> *names = [NSMutableDictionary dictionary];
    for (NSScreen *screen in [NSScreen screens]) {
        NSNumber *num = screen.deviceDescription[@"NSScreenNumber"];
        if (num) names[num] = screen.localizedName;
    }
    return names;
}

API_AVAILABLE(macos(12.3))
static void sck_log_displays(NSArray<SCDisplay *> *displays,
                             const MdCaptureDisplay *list, const char *sel) {
    MD_LOG_E("capture output \"%s\" not found; displays (select by index, id:N or name):%s",
             sel, displays.count ? "" : " none");
    for (NSUInteger i = 0; i < displays.count; i++) {
        CGRect b = CGDisplayBounds(list[i].id);
        MD_LOG_E("  %lu: id:%u \"%s\" %.0fx%.0f at (%.0f,%.0f)%s", (unsigned long)i,
                 list[i].id, list[i].name ? list[i].name : "",
                 b.size.width, b.size.height, b.origin.x, b.origin.y,
                 list[i].primary ? " main" : "");
    }
}

/* The display config.output selects, or nil (after logging the choices).
 * Default, index and id selections never touch AppKit: a watchdog restart
 * runs start on the capture thread, and NSScreen is a main-thread class.
 * Names are only looked up for a name selection or the error listing. */
API_AVAILABLE(macos(12.3))
static SCDisplay *sck_select_display(NSArray<SCDisplay *> *displays, const char *sel) {
    const NSUInteger count = displays.count;
    MdCaptureDisplay *list = calloc(count ? count : 1, sizeof(*list));
    if (!list) return nil;
    const CGDirectDisplayID main_id = CGMainDisplayID();
    for (NSUInteger i = 0; i < count; i++) {
        list[i].id = displays[i].displayID;
        list[i].primary = list[i].id == main_id;
    }
    int pick = md_capture_display_choose(list, count, sel);
    if (pick < 0 && sel && *sel) {
        /* Precise lifetime: list[].name borrows these strings' buffers. */
        __attribute__((objc_precise_lifetime))
        NSDictionary<NSNumber *, NSString *> *names = sck_display_names();
        for (NSUInteger i = 0; i < count; i++)
            list[i].name = names[@(list[i].id)].UTF8String;
        pick = md_capture_display_choose(list, count, sel);
        if (pick < 0) sck_log_displays(displays, list, sel);
    } else if (pick < 0) {
        MD_LOG_I("no displays found");
    }
    free(list);
    return pick < 0 ? nil : displays[(NSUInteger)pick];
}

/* Backing pixels per point: SCK's own figure on macOS 14+, else the current
 * display mode's pixel/point ratio (CoreGraphics, safe off the main thread,
 * unlike NSScreen.backingScaleFactor). 1 if neither is known. */
API_AVAILABLE(macos(12.3))
static double sck_backing_scale(SCDisplay *display, SCContentFilter *filter) {
    if (@available(macOS 14.0, *)) {
        if (filter.pointPixelScale > 0) return filter.pointPixelScale;
    }
    double scale = 1.0;
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display.displayID);
    if (mode) {
        const size_t pt = CGDisplayModeGetWidth(mode);
        const size_t px = CGDisplayModeGetPixelWidth(mode);
        if (pt && px) scale = (double)px / (double)pt;
        CGDisplayModeRelease(mode);
    }
    return scale;
}

/* sck_start/sck_stop are reachable only through screencapturekit_backend,
 * which md_capture_backend_create gates at runtime; the annotations let the
 * compiler prove that instead of re-checking per call. */
API_AVAILABLE(macos(12.3))
static int sck_start(MdCaptureCtx *ctx) {
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
        MD_LOG_E("failed to get shareable content: %s", contentError ? [[contentError localizedDescription] UTF8String] : "unknown error");
        return -1;
    }

    SCDisplay *display = sck_select_display(shareableContent.displays,
                                            ctx->config.output);
    if (!display) return -1;

    /* Content filter: the selected display, all windows */
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display
                                                      excludingWindows:@[]];

    /* Configure the stream at the display's native pixels. SCDisplay sizes
     * are points; capturing at them halves a Retina display's resolution.
     * ctx->output_rect stays in points for CGEvent (see capture.h). */
    const double scale = sck_backing_scale(display, filter);
    const size_t px_w = (size_t)lround((double)display.width * scale);
    const size_t px_h = (size_t)lround((double)display.height * scale);
    SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
    config.width = px_w;
    config.height = px_h;
    config.showsCursor = ctx->config.show_cursor;

    /* Request BGRA pixel format (most compatible with our encoder) */
    config.pixelFormat = kCVPixelFormatType_32BGRA;

    /* Frame rate */
    uint32_t fps = ctx->config.target_fps ? ctx->config.target_fps : 60;
    config.minimumFrameInterval = CMTimeMake(1, (int32_t)fps);

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
        MD_LOG_E("failed to add stream output: %s", [[addOutputError localizedDescription] UTF8String]);
        return -1;
    }

    /* Store references, dropping a previous (stopped) start's objects. */
    if (sck->stream)       CFRelease(sck->stream);
    if (sck->delegate)     CFRelease(sck->delegate);
    if (sck->captureQueue) CFRelease(sck->captureQueue);
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
        MD_LOG_E("failed to start capture: %s", [[startError localizedDescription] UTF8String]);
        return -1;
    }

    const CGRect bounds = CGDisplayBounds(display.displayID);
    ctx->output_rect = (MdCaptureRect){
        .x = (int32_t)bounds.origin.x, .y = (int32_t)bounds.origin.y,
        .width = (uint32_t)bounds.size.width, .height = (uint32_t)bounds.size.height,
    };
    ctx->active = true;
    ctx->width  = (uint32_t)px_w;
    ctx->height = (uint32_t)px_h;
    MD_LOG_I("capturing display id:%u at %zux%zu pixels (%ldx%ld points, scale %.2f)",
             display.displayID, px_w, px_h, (long)display.width,
             (long)display.height, scale);

    return 0;
}

static int sck_get_frame(MdCaptureCtx *ctx, MdFrame *out, uint32_t timeout_ms) {
    SCKState *sck = ctx->backend_data;
    if (!sck) return MD_CAPTURE_ERROR;

    uint64_t deadline = md_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    pthread_mutex_lock(&sck->frame_lock);
    while (!sck->frame_ready && ctx->active &&
           md_cond_timedwait_until(&sck->frame_cond, &sck->frame_lock,
                                   deadline) != ETIMEDOUT) {}

    if (!sck->frame_ready) {
        bool active = ctx->active;
        pthread_mutex_unlock(&sck->frame_lock);
        return active ? MD_CAPTURE_IDLE : MD_CAPTURE_ERROR;
    }

    *out = sck->pending_frame;
    sck->frame_ready = false;
    sck->frame_checked_out = true;
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
    sck->frame_checked_out = false;
    pthread_mutex_unlock(&sck->frame_lock);
}

API_AVAILABLE(macos(12.3))
static void sck_stop(MdCaptureCtx *ctx) {
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

API_AVAILABLE(macos(12.3))
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
    MD_LOG_I("ScreenCaptureKit requires macOS 12.3+");
    return NULL;
}
