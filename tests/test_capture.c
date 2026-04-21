/*
 * metadesk — tests/test_capture.c
 * Unit tests for the capture convenience API using a mock backend.
 *
 * The capture.c convenience API delegates to a vtable; we provide
 * a mock backend to test the platform-agnostic logic without requiring
 * PipeWire, ScreenCaptureKit, or DXGI.
 */
#include "capture.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASS(name) printf("  PASS  %s\n", name)

/* ── Mock backend state ──────────────────────────────────────── */

typedef struct {
    int  init_called;
    int  start_called;
    int  get_frame_count;
    int  release_count;
    int  stop_called;
    int  destroy_called;
    bool active;
    uint32_t width;
    uint32_t height;
} MockState;

static MockState g_mock;

/* Static pixel buffer for mock frames */
static uint8_t g_mock_pixels[640 * 480 * 4];

static int mock_init(MdCaptureCtx *ctx, const MdCaptureConfig *cfg)
{
    (void)cfg;
    MockState *ms = calloc(1, sizeof(MockState));
    if (!ms) return -1;
    ms->init_called = 1;
    ms->width  = 640;
    ms->height = 480;
    ctx->backend_data = ms;
    ctx->width  = ms->width;
    ctx->height = ms->height;
    /* Copy state to global for assertions */
    g_mock.init_called = 1;
    return 0;
}

static int mock_start(MdCaptureCtx *ctx)
{
    MockState *ms = ctx->backend_data;
    ms->start_called = 1;
    ms->active = true;
    ctx->active = true;
    g_mock.start_called = 1;
    return 0;
}

static int mock_get_frame(MdCaptureCtx *ctx, MdFrame *out)
{
    MockState *ms = ctx->backend_data;
    ms->get_frame_count++;
    g_mock.get_frame_count++;

    memset(out, 0, sizeof(*out));
    out->width      = ms->width;
    out->height     = ms->height;
    out->stride     = ms->width * 4;
    out->format     = MD_PIX_CAPTURE_RGBA;
    out->buf_type   = MD_BUF_CPU;
    out->data       = g_mock_pixels;
    out->data_size  = sizeof(g_mock_pixels);
    out->seq        = (uint32_t)ms->get_frame_count;
    return 0;
}

static void mock_release_frame(MdCaptureCtx *ctx, MdFrame *frame)
{
    (void)frame;
    MockState *ms = ctx->backend_data;
    ms->release_count++;
    g_mock.release_count++;
}

static void mock_stop(MdCaptureCtx *ctx)
{
    MockState *ms = ctx->backend_data;
    ms->stop_called = 1;
    ms->active = false;
    ctx->active = false;
    g_mock.stop_called = 1;
}

static void mock_destroy(MdCaptureCtx *ctx)
{
    g_mock.destroy_called = 1;
    free(ctx->backend_data);
    ctx->backend_data = NULL;
}

static const MdCaptureBackend mock_backend = {
    .init          = mock_init,
    .start         = mock_start,
    .get_frame     = mock_get_frame,
    .release_frame = mock_release_frame,
    .stop          = mock_stop,
    .destroy       = mock_destroy,
};

/* ── Helper: create ctx using mock backend directly ──────────── */

static MdCaptureCtx *create_mock_ctx(const MdCaptureConfig *cfg)
{
    MdCaptureCtx *ctx = calloc(1, sizeof(MdCaptureCtx));
    if (!ctx) return NULL;

    ctx->vtable = &mock_backend;
    if (cfg) {
        ctx->config = *cfg;
    } else {
        ctx->config.target_fps  = 60;
        ctx->config.show_cursor = true;
    }

    if (ctx->vtable->init(ctx, &ctx->config) != 0) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

/* ── Tests ───────────────────────────────────────────────────── */

static void test_null_safety(void)
{
    /* All convenience functions should handle NULL without crashing */
    assert(md_capture_start(NULL) == -1);
    assert(md_capture_get_frame(NULL, NULL) == -1);
    md_capture_release_frame(NULL, NULL);
    assert(md_capture_is_active(NULL) == false);
    assert(md_capture_get_size(NULL, NULL, NULL) == -1);
    md_capture_stop(NULL);
    md_capture_destroy(NULL); /* should not crash */

    PASS("null safety");
}

static void test_create_and_destroy(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    assert(ctx != NULL);
    assert(g_mock.init_called);

    md_capture_destroy(ctx);
    assert(g_mock.stop_called);
    assert(g_mock.destroy_called);

    PASS("create and destroy");
}

static void test_is_active(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    assert(ctx != NULL);

    /* Not active until started */
    assert(md_capture_is_active(ctx) == false);

    /* Start → active */
    assert(md_capture_start(ctx) == 0);
    assert(md_capture_is_active(ctx) == true);
    assert(g_mock.start_called);

    /* Stop → inactive */
    md_capture_stop(ctx);
    assert(md_capture_is_active(ctx) == false);

    md_capture_destroy(ctx);
    PASS("is_active lifecycle");
}

static void test_get_size(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    assert(ctx != NULL);

    uint32_t w = 0, h = 0;
    assert(md_capture_get_size(ctx, &w, &h) == 0);
    assert(w == 640);
    assert(h == 480);

    /* NULL output params */
    assert(md_capture_get_size(ctx, NULL, &h) == -1);
    assert(md_capture_get_size(ctx, &w, NULL) == -1);

    md_capture_destroy(ctx);
    PASS("get_size");
}

static void test_get_size_before_init(void)
{
    /* If width/height are zero, get_size returns -1 */
    MdCaptureCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    uint32_t w, h;
    assert(md_capture_get_size(&ctx, &w, &h) == -1);

    PASS("get_size before init (zero dimensions)");
}

static void test_get_frame_and_release(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    assert(ctx != NULL);
    assert(md_capture_start(ctx) == 0);

    MdFrame frame;
    assert(md_capture_get_frame(ctx, &frame) == 0);
    assert(frame.width == 640);
    assert(frame.height == 480);
    assert(frame.stride == 640 * 4);
    assert(frame.format == MD_PIX_CAPTURE_RGBA);
    assert(frame.buf_type == MD_BUF_CPU);
    assert(frame.data != NULL);
    assert(frame.seq == 1);
    assert(g_mock.get_frame_count == 1);

    md_capture_release_frame(ctx, &frame);
    assert(g_mock.release_count == 1);

    /* Second frame increments seq */
    assert(md_capture_get_frame(ctx, &frame) == 0);
    assert(frame.seq == 2);
    assert(g_mock.get_frame_count == 2);

    md_capture_release_frame(ctx, &frame);
    assert(g_mock.release_count == 2);

    md_capture_destroy(ctx);
    PASS("get_frame and release");
}

static void test_config_passthrough(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureConfig cfg = {
        .target_fps  = 30,
        .show_cursor = false,
    };

    MdCaptureCtx *ctx = create_mock_ctx(&cfg);
    assert(ctx != NULL);
    assert(ctx->config.target_fps == 30);
    assert(ctx->config.show_cursor == false);

    md_capture_destroy(ctx);
    PASS("config passthrough");
}

static void test_default_config(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    assert(ctx != NULL);
    assert(ctx->config.target_fps == 60);
    assert(ctx->config.show_cursor == true);

    md_capture_destroy(ctx);
    PASS("default config");
}

static void test_stop_idempotent(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    assert(ctx != NULL);
    assert(md_capture_start(ctx) == 0);

    md_capture_stop(ctx);
    assert(g_mock.stop_called == 1);

    /* destroy calls stop again — should not crash */
    md_capture_destroy(ctx);
    PASS("stop idempotent");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_capture: capture convenience API tests (mock backend)\n");

    test_null_safety();
    test_create_and_destroy();
    test_is_active();
    test_get_size();
    test_get_size_before_init();
    test_get_frame_and_release();
    test_config_passthrough();
    test_default_config();
    test_stop_idempotent();

    printf("\nAll capture tests passed.\n");
    return 0;
}
