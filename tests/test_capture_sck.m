/*
 * metadesk — tests/test_capture_sck.m
 * ScreenCaptureKit backend: a stream the system stops (SCStreamDelegate
 * stream:didStopWithError:) is reported broken (md-2p10).
 *
 * Drives the production delegate through a test seam on an initialised but
 * unstarted context, so it needs no Screen Recording permission and never
 * captures: the seam binds a delegate the way sck_start does, and the test
 * calls its delegate method as SCK would.
 */
#include "capture.h"
#include "log.h"
#include "md_time.h"
#include "md_test.h"

#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <pthread.h>
#include <string.h>

extern void *md_capture_sck_test_bind_delegate(MdCaptureCtx *ctx);

/* ── Captured log lines ──────────────────────────────────────── */

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_log[4096];

static void capture_sink(MdLogLevel level, const char *tag, const char *msg, void *ud)
{
    (void)level; (void)ud;
    pthread_mutex_lock(&g_log_mu);
    size_t n = strlen(g_log);
    snprintf(g_log + n, sizeof(g_log) - n, "[%s] %s\n", tag, msg);
    pthread_mutex_unlock(&g_log_mu);
}

static void log_clear(void)
{
    pthread_mutex_lock(&g_log_mu);
    g_log[0] = '\0';
    pthread_mutex_unlock(&g_log_mu);
}

static bool log_has(const char *needle)
{
    pthread_mutex_lock(&g_log_mu);
    bool found = strstr(g_log, needle) != NULL;
    pthread_mutex_unlock(&g_log_mu);
    return found;
}

/* ── Helpers ─────────────────────────────────────────────────── */

API_AVAILABLE(macos(12.3))
static id<SCStreamDelegate> bind_delegate(MdCaptureCtx *ctx)
{
    return (__bridge id<SCStreamDelegate>)md_capture_sck_test_bind_delegate(ctx);
}

API_AVAILABLE(macos(12.3))
static void system_stop(id<SCStreamDelegate> d, long code)
{
    [d stream:(SCStream *_Nonnull)nil
        didStopWithError:[NSError errorWithDomain:SCStreamErrorDomain
                                             code:code
                                         userInfo:nil]];
}

static int poll_frame(MdCaptureCtx *ctx)
{
    MdFrame f;
    int r = md_capture_get_frame(ctx, &f, 0);
    if (r == 0) md_capture_release_frame(ctx, &f);
    return r;
}

static MdCaptureCtx *new_ctx(void)
{
    MdCaptureCtx *ctx = md_capture_create(NULL);
    /* What a successful start leaves set; get_frame then means "healthy". */
    if (ctx) ctx->active = true;
    return ctx;
}

/* ── Tests ───────────────────────────────────────────────────── */

API_AVAILABLE(macos(12.3))
static void test_system_stop_reports_error(void)
{
    TEST("system stop -> broken -> MD_CAPTURE_ERROR, promptly");
    MdCaptureCtx *ctx = new_ctx();
    MD_CHECK(ctx != NULL);
    if (!ctx) return;
    id<SCStreamDelegate> d = bind_delegate(ctx);
    MD_CHECK(d != nil);
    MD_CHECK([d respondsToSelector:@selector(stream:didStopWithError:)]);

    /* Healthy, no frames: idle, not broken. */
    MD_CHECK(poll_frame(ctx) == MD_CAPTURE_IDLE);

    /* The stop arrives on another queue while the consumer is blocked in a
     * long get_frame: it must wake and fail at once, not idle out. */
    log_clear();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                   dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        system_stop(d, -3815 /* SCStreamErrorNoCaptureSource */);
    });
    const uint64_t t0 = md_monotonic_ns();
    MdFrame f;
    MD_CHECK(md_capture_get_frame(ctx, &f, 10000) == MD_CAPTURE_ERROR);
    MD_CHECK(md_monotonic_ns() - t0 < 2000ull * 1000000ull);

    /* Stays broken until a restart, without waiting out the timeout. */
    const uint64_t t1 = md_monotonic_ns();
    MD_CHECK(md_capture_get_frame(ctx, &f, 10000) == MD_CAPTURE_ERROR);
    MD_CHECK(md_monotonic_ns() - t1 < 1000ull * 1000000ull);

    /* Domain, code and meaning are logged. */
    MD_CHECK(log_has("capture stream ended by ScreenCaptureKit"));
    MD_CHECK(log_has("SCStreamErrorDomain -3815 NoCaptureSource"));
    MD_CHECK(log_has("the captured display is gone"));

    /* A restart binds a fresh delegate and clears the fault. */
    d = bind_delegate(ctx);
    MD_CHECK(poll_frame(ctx) == MD_CAPTURE_IDLE);

    md_capture_destroy(ctx);
    PASS();
}

API_AVAILABLE(macos(12.3))
static void test_permission_revoked_logged(void)
{
    TEST("revoked Screen Recording permission: clear log line");
    MdCaptureCtx *ctx = new_ctx();
    MD_CHECK(ctx != NULL);
    if (!ctx) return;
    id<SCStreamDelegate> d = bind_delegate(ctx);
    log_clear();
    system_stop(d, -3801 /* SCStreamErrorUserDeclined */);
    MD_CHECK(poll_frame(ctx) == MD_CAPTURE_ERROR);
    MD_CHECK(log_has("SCStreamErrorDomain -3801 UserDeclined"));
    MD_CHECK(log_has("Screen Recording permission revoked"));
    MD_CHECK(log_has("System Settings > Privacy & Security"));

    /* Other domains and codes are still logged and still fatal. */
    d = bind_delegate(ctx);
    log_clear();
    [d stream:(SCStream *_Nonnull)nil
        didStopWithError:[NSError errorWithDomain:NSOSStatusErrorDomain
                                             code:-12345
                                         userInfo:nil]];
    MD_CHECK(poll_frame(ctx) == MD_CAPTURE_ERROR);
    MD_CHECK(log_has("NSOSStatusErrorDomain -12345"));
    md_capture_destroy(ctx);
    PASS();
}

API_AVAILABLE(macos(12.3))
static void test_stale_and_late_stops_ignored(void)
{
    TEST("stop from a previous start or after destroy is a no-op");
    MdCaptureCtx *ctx = new_ctx();
    MD_CHECK(ctx != NULL);
    if (!ctx) return;
    id<SCStreamDelegate> old = bind_delegate(ctx);
    id<SCStreamDelegate> cur = bind_delegate(ctx); /* restart: old is detached */
    MD_CHECK(old != cur);
    log_clear();
    system_stop(old, -3821 /* SCStreamErrorSystemStoppedStream */);
    MD_CHECK(poll_frame(ctx) == MD_CAPTURE_IDLE);
    MD_CHECK(!log_has("capture stream ended"));

    /* The stream may outlive the backend state: a stop reported after
     * destroy must not touch the freed state. `cur` stays retained here. */
    md_capture_destroy(ctx);
    system_stop(cur, -3821);
    MD_CHECK(!log_has("capture stream ended"));
    PASS();
}

int main(void)
{
    printf("test_capture_sck\n");
    md_log_set_sink(capture_sink, NULL);
    if (@available(macOS 12.3, *)) {
        @autoreleasepool {
            RUN_TEST(test_system_stop_reports_error);
            RUN_TEST(test_permission_revoked_logged);
            RUN_TEST(test_stale_and_late_stops_ignored);
        }
    } else {
        printf("  SKIP: ScreenCaptureKit requires macOS 12.3+\n");
    }
    md_log_set_sink(NULL, NULL);
    return md_test_report();
}
