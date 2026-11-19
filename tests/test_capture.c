/*
 * metadesk — tests/test_capture.c
 * Unit tests for the capture convenience API using a mock backend.
 *
 * The capture.c convenience API delegates to a vtable; we provide
 * a mock backend to test the platform-agnostic logic without requiring
 * PipeWire, ScreenCaptureKit, or DXGI.
 */
#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md_test.h"
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#ifdef __linux__
#include <dbus/dbus.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <unistd.h>
#endif


/* ── Mock backend state ──────────────────────────────────────── */

typedef struct {
    int  init_called;
    int  start_called;
    int  get_frame_count;
    int  release_count;
    int  stop_called;
    int  destroy_called;
    bool active;
    int frame_result;
    int start_result;
    bool null_cpu_data;
    uint32_t width;
    uint32_t height;
    uint32_t last_timeout_ms;
    uint32_t last_size_timeout_ms;
    int wait_size_calls;
} MockState;

static MockState g_mock;

/* Static pixel buffer for mock frames */
static uint8_t g_mock_pixels[640 * 480 * 4];

static int mock_init(MdCaptureCtx *ctx)
{
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
    if (ms->start_result) return ms->start_result;
    ms->active = true;
    ctx->active = true;
    g_mock.start_called = 1;
    return 0;
}

static int mock_get_frame(MdCaptureCtx *ctx, MdFrame *out, uint32_t timeout_ms)
{
    MockState *ms = ctx->backend_data;
    ms->last_timeout_ms = timeout_ms;
    ms->get_frame_count++;
    g_mock.get_frame_count++;
    if (ms->frame_result) return ms->frame_result;

    memset(out, 0, sizeof(*out));
    out->width      = ms->width;
    out->height     = ms->height;
    out->stride     = ms->width * 4;
    out->format     = MD_PIX_CAPTURE_RGBA;
    out->buf_type   = MD_BUF_CPU;
    out->data       = ms->null_cpu_data ? NULL : g_mock_pixels;
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

/* Asynchronous-size backend: reports the size only once "negotiated". */
static int mock_wait_size(MdCaptureCtx *ctx, uint32_t timeout_ms,
                          uint32_t *width, uint32_t *height)
{
    MockState *ms = ctx->backend_data;
    ms->wait_size_calls++;
    ms->last_size_timeout_ms = timeout_ms;
    if (!ms->active) return -1;
    *width = ms->width;
    *height = ms->height;
    return 0;
}

static const MdCaptureBackend mock_async_size_backend = {
    .init          = mock_init,
    .start         = mock_start,
    .get_frame     = mock_get_frame,
    .release_frame = mock_release_frame,
    .stop          = mock_stop,
    .destroy       = mock_destroy,
    .wait_size     = mock_wait_size,
};

/* ── Helper: create ctx using mock backend directly ──────────── */

static MdCaptureCtx *create_backend_ctx(const MdCaptureBackend *backend,
                                        const MdCaptureConfig *cfg)
{
    MdCaptureCtx *ctx = calloc(1, sizeof(MdCaptureCtx));
    if (!ctx) return NULL;

    ctx->vtable = backend;
    if (cfg) {
        ctx->config = *cfg;
    } else {
        ctx->config.target_fps  = 60;
        ctx->config.show_cursor = true;
    }

    if (ctx->vtable->init(ctx) != 0) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

static MdCaptureCtx *create_mock_ctx(const MdCaptureConfig *cfg)
{
    return create_backend_ctx(&mock_backend, cfg);
}

/* ── Tests ───────────────────────────────────────────────────── */

static void test_null_safety(void)
{
    /* All convenience functions should handle NULL without crashing */
    MD_CHECK(md_capture_start(NULL) == -1);
    MD_CHECK(md_capture_get_frame(NULL, NULL, 0) == -1);
    md_capture_release_frame(NULL, NULL);
    MD_CHECK(md_capture_is_active(NULL) == false);
    MD_CHECK(md_capture_get_size(NULL, NULL, NULL) == -1);
    MD_CHECK(md_capture_wait_size(NULL, 10, NULL, NULL) == -1);
    md_capture_stop(NULL);
    md_capture_destroy(NULL); /* should not crash */

    PASS("null safety");
}

static void test_create_and_destroy(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    MD_CHECK(ctx != NULL);
    MD_CHECK(g_mock.init_called);

    md_capture_destroy(ctx);
    MD_CHECK(g_mock.stop_called);
    MD_CHECK(g_mock.destroy_called);

    PASS("create and destroy");
}

static void test_is_active(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    MD_CHECK(ctx != NULL);

    /* Not active until started */
    MD_CHECK(md_capture_is_active(ctx) == false);

    /* Start → active */
    MD_CHECK(md_capture_start(ctx) == 0);
    MD_CHECK(md_capture_is_active(ctx) == true);
    MD_CHECK(g_mock.start_called);

    /* Stop → inactive */
    md_capture_stop(ctx);
    MD_CHECK(md_capture_is_active(ctx) == false);

    md_capture_destroy(ctx);
    PASS("is_active lifecycle");
}

static void test_get_size(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    MD_CHECK(ctx != NULL);

    uint32_t w = 0, h = 0;
    MD_CHECK(md_capture_get_size(ctx, &w, &h) == 0);
    MD_CHECK(w == 640);
    MD_CHECK(h == 480);

    /* NULL output params */
    MD_CHECK(md_capture_get_size(ctx, NULL, &h) == -1);
    MD_CHECK(md_capture_get_size(ctx, &w, NULL) == -1);

    md_capture_destroy(ctx);
    PASS("get_size");
}

/* Synthetic display list modelled on a two-monitor Mac: an external main
 * display listed second, as SCShareableContent may order them. */
static void test_display_choose(void)
{
    const MdCaptureDisplay displays[] = {
        { .id = 1,        .name = "Built-in Retina Display", .primary = false },
        { .id = 69732928, .name = "DELL U2720Q",             .primary = true  },
        { .id = 4128833,  .name = NULL,                      .primary = false },
    };
    const size_t n = sizeof(displays) / sizeof(displays[0]);

    /* Default: the flagged primary, else index 0. */
    MD_CHECK(md_capture_display_choose(displays, n, NULL) == 1);
    MD_CHECK(md_capture_display_choose(displays, n, "") == 1);
    const MdCaptureDisplay unflagged[] = { { .id = 7 }, { .id = 8 } };
    MD_CHECK(md_capture_display_choose(unflagged, 2, NULL) == 0);

    /* Bare digits are always an index, never an id (id 1 is a real one). */
    MD_CHECK(md_capture_display_choose(displays, n, "0") == 0);
    MD_CHECK(md_capture_display_choose(displays, n, "2") == 2);
    MD_CHECK(md_capture_display_choose(displays, n, "0002") == 2);
    MD_CHECK(md_capture_display_choose(displays, n, "3") == -1);
    MD_CHECK(md_capture_display_choose(displays, n, "69732928") == -1);

    /* id:N, decimal or hex, case-insensitive prefix. */
    MD_CHECK(md_capture_display_choose(displays, n, "id:1") == 0);
    MD_CHECK(md_capture_display_choose(displays, n, "id:69732928") == 1);
    MD_CHECK(md_capture_display_choose(displays, n, "ID:0x3F0041") == 2);
    MD_CHECK(md_capture_display_choose(displays, n, "id:2") == -1);
    MD_CHECK(md_capture_display_choose(displays, n, "id:") == -1);
    MD_CHECK(md_capture_display_choose(displays, n, "id:-1") == -1);
    MD_CHECK(md_capture_display_choose(displays, n, "id:1x") == -1);
    MD_CHECK(md_capture_display_choose(displays, n, "id:4294967297") == -1);

    /* Names: exact, case-insensitive; a nameless display never matches. */
    MD_CHECK(md_capture_display_choose(displays, n, "dell u2720q") == 1);
    MD_CHECK(md_capture_display_choose(displays, n, "Built-in Retina Display") == 0);
    MD_CHECK(md_capture_display_choose(displays, n, "DELL") == -1);
    MD_CHECK(md_capture_display_choose(displays, n, "\\\\.\\DISPLAY1") == -1);

    /* Duplicate names are ambiguous; index and id still disambiguate. */
    const MdCaptureDisplay twins[] = {
        { .id = 10, .name = "LG UltraFine", .primary = true },
        { .id = 11, .name = "LG UltraFine" },
    };
    MD_CHECK(md_capture_display_choose(twins, 2, "LG UltraFine") == -1);
    MD_CHECK(md_capture_display_choose(twins, 2, "id:11") == 1);

    /* No displays: nothing to pick, even by default. */
    MD_CHECK(md_capture_display_choose(displays, 0, NULL) == -1);
    MD_CHECK(md_capture_display_choose(NULL, n, "0") == -1);
    PASS("display_choose");
}

static void test_output_rect(void)
{
    memset(&g_mock, 0, sizeof(g_mock));
    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    MD_CHECK(ctx != NULL);
    MdCaptureRect r = { 0 };
    MD_CHECK(md_capture_get_output_rect(NULL, &r) == -1);
    MD_CHECK(md_capture_get_output_rect(ctx, NULL) == -1);
    MD_CHECK(md_capture_get_output_rect(ctx, &r) == -1); /* not published */
    ctx->output_rect = (MdCaptureRect){ .x = -1920, .y = 120, .width = 1920, .height = 1080 };
    MD_CHECK(md_capture_get_output_rect(ctx, &r) == 0);
    MD_CHECK(r.x == -1920 && r.y == 120 && r.width == 1920 && r.height == 1080);
    md_capture_destroy(ctx);
    PASS("output_rect");
}

static void test_get_size_before_init(void)
{
    /* If width/height are zero, get_size returns -1 */
    MdCaptureCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    uint32_t w, h;
    MD_CHECK(md_capture_get_size(&ctx, &w, &h) == -1);
    MD_CHECK(md_capture_wait_size(&ctx, 50, &w, &h) == -1);

    PASS("get_size before init (zero dimensions)");
}

static void test_wait_size_dispatch(void)
{
    /* Synchronous-size backend: ctx dimensions answer immediately. */
    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    MD_CHECK(ctx != NULL);
    uint32_t w = 0, h = 0;
    MD_CHECK(md_capture_wait_size(ctx, 2000, &w, &h) == 0 && w == 640 && h == 480);
    MD_CHECK(md_capture_wait_size(ctx, 2000, NULL, &h) == -1);
    md_capture_destroy(ctx);

    /* Asynchronous backend owns the wait; get_size is its zero-timeout poll. */
    ctx = create_backend_ctx(&mock_async_size_backend, NULL);
    MD_CHECK(ctx != NULL);
    MockState *ms = ctx->backend_data;
    w = h = 0;
    MD_CHECK(md_capture_wait_size(ctx, 1234, &w, &h) == -1 && w == 0);
    MD_CHECK(ms->wait_size_calls == 1 && ms->last_size_timeout_ms == 1234);
    MD_CHECK(md_capture_start(ctx) == 0);
    MD_CHECK(md_capture_get_size(ctx, &w, &h) == 0 && w == 640 && h == 480);
    MD_CHECK(ms->wait_size_calls == 2 && ms->last_size_timeout_ms == 0);
    md_capture_destroy(ctx);
    PASS("wait_size: synchronous ctx size vs backend-owned negotiation wait");
}

static void test_get_frame_and_release(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    MD_CHECK(ctx != NULL);
    MD_CHECK(md_capture_start(ctx) == 0);

    MdFrame frame;
    MD_CHECK(md_capture_get_frame(ctx, &frame, 250) == 0);
    MD_CHECK(((MockState *)ctx->backend_data)->last_timeout_ms == 250);
    MD_CHECK(frame.width == 640);
    MD_CHECK(frame.height == 480);
    MD_CHECK(frame.stride == 640 * 4);
    MD_CHECK(frame.format == MD_PIX_CAPTURE_RGBA);
    MD_CHECK(frame.buf_type == MD_BUF_CPU);
    MD_CHECK(frame.data != NULL);
    MD_CHECK(frame.seq == 1);
    MD_CHECK(g_mock.get_frame_count == 1);

    md_capture_release_frame(ctx, &frame);
    MD_CHECK(g_mock.release_count == 1);

    /* Second frame increments seq */
    MD_CHECK(md_capture_get_frame(ctx, &frame, 0) == 0);
    MD_CHECK(((MockState *)ctx->backend_data)->last_timeout_ms == 0);
    MD_CHECK(frame.seq == 2);
    MD_CHECK(g_mock.get_frame_count == 2);

    md_capture_release_frame(ctx, &frame);
    MD_CHECK(g_mock.release_count == 2);

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
    MD_CHECK(ctx != NULL);
    MD_CHECK(ctx->config.target_fps == 30);
    MD_CHECK(ctx->config.show_cursor == false);

    md_capture_destroy(ctx);
    PASS("config passthrough");
}

static void test_default_config(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    MD_CHECK(ctx != NULL);
    MD_CHECK(ctx->config.target_fps == 60);
    MD_CHECK(ctx->config.show_cursor == true);

    md_capture_destroy(ctx);
    PASS("default config");
}

static void test_stop_idempotent(void)
{
    memset(&g_mock, 0, sizeof(g_mock));

    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    MD_CHECK(ctx != NULL);
    MD_CHECK(md_capture_start(ctx) == 0);

    md_capture_stop(ctx);
    MD_CHECK(g_mock.stop_called == 1);

    /* destroy calls stop again — should not crash */
    md_capture_destroy(ctx);
    PASS("stop idempotent");
}

static void test_invalid_cpu_frame(void)
{
    MdCaptureCtx *ctx = create_mock_ctx(NULL);
    MD_CHECK(ctx != NULL);
    MockState *ms = ctx->backend_data;
    MD_CHECK(md_capture_start(ctx) == 0);
    MdFrame frame;
    ms->null_cpu_data = true;
    MD_CHECK(md_capture_get_frame(ctx, &frame, 100) == MD_CAPTURE_ERROR);
    MD_CHECK(ms->release_count == 1 && frame.data == NULL && frame.data_size == 0);
    ms->null_cpu_data = false;
    MD_CHECK(md_capture_get_frame(ctx, &frame, 100) == 0 && frame.data != NULL);
    md_capture_release_frame(ctx, &frame);
    MD_CHECK(ms->release_count == 2);
    md_capture_destroy(ctx);
    PASS("NULL CPU frame is rejected/released; subsequent valid frame recovers");
}

static void test_watchdog(void)
{
    MdCaptureWatchdog w = {0};
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_IDLE, true, 5000, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(!w.armed && w.restarts == 0);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, 6, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, 10, 4, 2) == MD_CAPTURE_RECOVERY_RESTART);
    MD_CHECK(w.restarts == 1);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_IDLE, true, 11, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(!w.armed && w.restarts == 1);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, 12, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, 16, 4, 2) == MD_CAPTURE_RECOVERY_RESTART);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_IDLE, true, 17, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, 18, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, 22, 4, 2) == MD_CAPTURE_RECOVERY_EXHAUSTED);
    MD_CHECK(w.gave_up && w.restarts == 2);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, 30, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(md_capture_watchdog_step(&w, 0, true, 31, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(!w.gave_up && !w.armed && w.restarts == 0);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, UINT32_MAX-1, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, 1, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, true, 2, 4, 2) == MD_CAPTURE_RECOVERY_RESTART);
    MD_CHECK(md_capture_watchdog_step(&w, MD_CAPTURE_ERROR, false, 3, 4, 2) == MD_CAPTURE_RECOVERY_NONE);
    MD_CHECK(!w.armed && w.restarts == 0);
    PASS("host watchdog: idle, bounded broken retries, frame recovery, timer wrap");
}

static void test_capture_policy(void)
{
    MdCaptureConfig cfg = {0};
    MD_CHECK(md_capture_consent_timeout_ms(&cfg) == 180000);
    MD_CHECK(!md_capture_stall_may_forget(&cfg));
    cfg.forget_authorization_on_stall = true;
    MD_CHECK(md_capture_stall_may_forget(&cfg));
    cfg.unattended = true;
    MD_CHECK(!md_capture_stall_may_forget(&cfg));
    MD_CHECK(md_capture_consent_timeout_ms(&cfg) == 10000);
    cfg.consent_timeout_ms = UINT32_MAX;
    MD_CHECK(md_capture_consent_timeout_ms(&cfg) == 10000);
    cfg.consent_timeout_ms = 250;
    MD_CHECK(md_capture_consent_timeout_ms(&cfg) == 250);

    MdCaptureCtx *ctx = create_mock_ctx(&cfg);
    MockState *ms = ctx->backend_data;
    ms->start_result = -1;
    MD_CHECK(md_capture_start(ctx) == -1);
    MD_CHECK(!md_capture_is_active(ctx));
    ms->start_result = 0;
    MD_CHECK(md_capture_start(ctx) == 0);
    MdFrame frame;
    ms->frame_result = MD_CAPTURE_IDLE;
    MD_CHECK(md_capture_get_frame(ctx, &frame, 100) == MD_CAPTURE_IDLE);
    MD_CHECK(md_capture_is_active(ctx));
    ms->frame_result = MD_CAPTURE_ERROR;
    MD_CHECK(md_capture_get_frame(ctx, &frame, 100) == MD_CAPTURE_ERROR);
    md_capture_destroy(ctx);
    PASS("fail-closed start, idle vs error, unattended policy");
}

#ifndef _WIN32
static void test_restore_tokens(void)
{
    char root[] = "/tmp/metadesk-capture-XXXXXX";
    MD_CHECK(mkdtemp(root) != NULL);
    const char *old = getenv("XDG_STATE_HOME");
    char *saved = old ? strdup(old) : NULL;
    MD_CHECK(setenv("XDG_STATE_HOME", root, 1) == 0);
    char dir[512], path[600], buf[1024];
    snprintf(dir, sizeof(dir), "%s/metadesk", root);
    snprintf(path, sizeof(path), "%s/screencast.token", dir);
    mode_t mask = umask(0);
    MD_CHECK(md_capture_restore_token_load(buf, sizeof(buf)) == 0);
    MD_CHECK(md_capture_restore_token_save("old-opaque-token") == 0);
    struct stat st;
    MD_CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    MD_CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    ino_t first = st.st_ino;
    MD_CHECK(md_capture_restore_token_save("new-opaque-token") == 0);
    MD_CHECK(stat(path, &st) == 0 && st.st_ino != first);
    MD_CHECK(md_capture_restore_token_load(buf, sizeof(buf)) == strlen("new-opaque-token"));
    MD_CHECK(strcmp(buf, "new-opaque-token") == 0);

    /* Readable legacy format and permissions are preserved/tightened. */
    FILE *f = fopen(path, "w");
    MD_CHECK(f != NULL);
    fputs("legacy-token\r\n", f);
    fclose(f);
    MD_CHECK(chmod(dir, 0755) == 0 && chmod(path, 0644) == 0);
    MD_CHECK(md_capture_restore_token_load(buf, sizeof(buf)) == strlen("legacy-token"));
    MD_CHECK(strcmp(buf, "legacy-token") == 0);
    MD_CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    MD_CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    /* Force a partial temp write: the old token must remain intact. */
    pid_t child = fork();
    MD_CHECK(child >= 0);
    if (child == 0) {
        struct rlimit limit = {4, 4};
        signal(SIGXFSZ, SIG_IGN);
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(2);
        _exit(md_capture_restore_token_save("replacement-must-not-truncate-old") == -1 ? 0 : 3);
    }
    int status;
    MD_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    MD_CHECK(md_capture_restore_token_load(buf, sizeof(buf)) == strlen("legacy-token"));
    MD_CHECK(strcmp(buf, "legacy-token") == 0);
    DIR *listing = opendir(dir);
    MD_CHECK(listing != NULL);
    struct dirent *entry;
    while ((entry = readdir(listing))) MD_CHECK(strncmp(entry->d_name, ".screencast.token.", 17) != 0);
    closedir(listing);

    /* Atomic replacement: concurrent readers see complete old or new content. */
    MD_CHECK(md_capture_restore_token_save("aaaaaaaaaaaaaaaa") == 0);
    child = fork();
    MD_CHECK(child >= 0);
    if (child == 0) {
        for (int i = 0; i < 2000; i++) {
            char token[64];
            size_t n = md_capture_restore_token_load(token, sizeof(token));
            if (n != 16 || (strcmp(token, "aaaaaaaaaaaaaaaa") && strcmp(token, "bbbbbbbbbbbbbbbb"))) _exit(4);
        }
        _exit(0);
    }
    for (int i = 0; i < 100; i++)
        MD_CHECK(md_capture_restore_token_save(i % 2 ? "aaaaaaaaaaaaaaaa" : "bbbbbbbbbbbbbbbb") == 0);
    MD_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Symlink content is never read or chmod'd. Atomic save replaces the link. */
    MD_CHECK(unlink(path) == 0);
    MD_CHECK(symlink("/dev/null", path) == 0);
    MD_CHECK(md_capture_restore_token_load(buf, sizeof(buf)) == 0);
    MD_CHECK(md_capture_restore_token_save("safe") == 0);
    MD_CHECK(lstat(path, &st) == 0 && S_ISREG(st.st_mode));
    MD_CHECK(md_capture_restore_token_forget() == 0);
    MD_CHECK(md_capture_restore_token_load(buf, sizeof(buf)) == 0);
    MD_CHECK(md_capture_restore_token_forget() == 0);
    MD_CHECK(rmdir(dir) == 0);
    MD_CHECK(symlink(root, dir) == 0);
    MD_CHECK(md_capture_restore_token_save("unsafe-directory") == -1);
    MD_CHECK(unlink(dir) == 0);
    MD_CHECK(rmdir(root) == 0);
    umask(mask);
    if (saved) { setenv("XDG_STATE_HOME", saved, 1); free(saved); }
    else unsetenv("XDG_STATE_HOME");
    PASS("token modes, rotation, partial-write rollback, atomic readers, symlinks, revocation");
}
#endif

#ifdef __linux__
static double test_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void test_stalled_bus(void) {
    char root[] = "/tmp/md-capture-bus-XXXXXX";
    MD_CHECK(mkdtemp(root) != NULL);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/bus", root);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    MD_CHECK(listener >= 0);
    MD_CHECK(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    MD_CHECK(listen(listener, 1) == 0);
    pid_t child = fork();
    MD_CHECK(child >= 0);
    if (child == 0) {
        int peer = accept(listener, NULL, NULL);
        char bytes[256];
        /* Deliberately never complete authentication; EOF is event-driven. */
        while (peer >= 0 && read(peer, bytes, sizeof(bytes)) > 0) {}
        if (peer >= 0) close(peer);
        close(listener);
        _exit(0);
    }
    const char *old = getenv("DBUS_SESSION_BUS_ADDRESS");
    char *saved = old ? strdup(old) : NULL;
    char address[256];
    snprintf(address, sizeof(address), "unix:path=%s", addr.sun_path);
    setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
    MdCaptureConfig cfg = {.unattended = true, .consent_timeout_ms = 150};
    MdCaptureCtx *ctx = md_capture_create(&cfg);
    MD_CHECK(ctx != NULL);
    double start = test_now();
    MD_CHECK(md_capture_start(ctx) == -1);
    double elapsed = test_now() - start;
    MD_CHECK(elapsed >= .1 && elapsed < 1.5);
    md_capture_destroy(ctx);
    int status;
    MD_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(listener);
    unlink(addr.sun_path);
    rmdir(root);
    if (saved) { setenv("DBUS_SESSION_BUS_ADDRESS", saved, 1); free(saved); }
    else unsetenv("DBUS_SESSION_BUS_ADDRESS");
    PASS("stalled private bus authentication consumes consent budget");
}

/* Minimal portal fixture on a new private dbus-daemon, never the desktop bus. */
static void mock_portal(int ready, int mode) {
    DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SESSION, NULL);
    if (!bus || dbus_bus_request_name(bus, "org.freedesktop.portal.Desktop", 0, NULL) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
        _exit(20);
    if (write(ready, "R", 1) != 1) _exit(21);
    while (dbus_connection_read_write(bus, -1)) {
        DBusMessage *call;
        while ((call = dbus_connection_pop_message(bus))) {
            const char *method = dbus_message_get_member(call);
            if (dbus_message_is_method_call(call, "org.freedesktop.portal.Session", "Close")) {
                if (write(ready, "X", 1) != 1) _exit(24);
                DBusMessage *closed = dbus_message_new_method_return(call);
                dbus_connection_send(bus, closed, NULL);
                dbus_message_unref(closed);
                dbus_message_unref(call);
                dbus_connection_flush(bus);
                continue;
            }
            if (dbus_message_get_type(call) != DBUS_MESSAGE_TYPE_METHOD_CALL || !method ||
                (strcmp(method, "CreateSession") && strcmp(method, "SelectSources") && strcmp(method, "Start"))) {
                dbus_message_unref(call);
                continue;
            }
            char path[512], sender[128];
            const char *source = dbus_message_get_sender(call);
            snprintf(sender, sizeof(sender), "%s", source + 1);
            for (char *p = sender; *p; p++) if (*p == '.') *p = '_';
            int seq = !strcmp(method, "CreateSession") ? 0 : !strcmp(method, "SelectSources") ? 1 : 2;
            snprintf(path, sizeof(path), "/org/freedesktop/portal/desktop/request/%s/metadesk%d", sender, seq);
            char event = (char)('0' + seq);
            if (write(ready, &event, 1) != 1) _exit(23);
            const char *request = path;
            DBusMessage *reply = dbus_message_new_method_return(call);
            dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &request, DBUS_TYPE_INVALID);
            dbus_connection_send(bus, reply, NULL);
            dbus_message_unref(reply);
            if ((mode == 2 || mode == 5) && seq == 2) { /* Start never responds. */
                /* Mode 5: steady unrelated Response chatter, from another
                 * connection, for longer than the consent budget. The portal
                 * itself stays responsive, so elapsed time measures only the
                 * client's wait: a deadline neither expires early per message
                 * (iteration counting) nor stretches past its budget. */
                if (mode == 5 && fork() == 0) {
                    DBusConnection *other_bus = dbus_bus_get_private(DBUS_BUS_SESSION, NULL);
                    for (int i = 0; other_bus && i < 300; i++) {
                        char other[600];
                        snprintf(other, sizeof(other), "%s_other%d", path, i);
                        DBusMessage *noise = dbus_message_new_signal(other,
                            "org.freedesktop.portal.Request", "Response");
                        dbus_message_set_destination(noise, source);
                        dbus_uint32_t code = 0;
                        dbus_message_append_args(noise, DBUS_TYPE_UINT32, &code, DBUS_TYPE_INVALID);
                        dbus_connection_send(other_bus, noise, NULL);
                        dbus_message_unref(noise);
                        dbus_connection_flush(other_bus);
                        const struct timespec pace = {.tv_nsec = 2000000}; /* producer pacing */
                        nanosleep(&pace, NULL);
                    }
                    _exit(0);
                }
                dbus_message_unref(call);
                dbus_connection_flush(bus);
                continue;
            }
            DBusMessage *signal = dbus_message_new_signal(path, "org.freedesktop.portal.Request", "Response");
            dbus_message_set_destination(signal, source);
            if (mode == 1) {
                const char *bad = "not-a-response-code";
                dbus_message_append_args(signal, DBUS_TYPE_STRING, &bad, DBUS_TYPE_INVALID);
            } else {
                DBusMessageIter args, dict;
                dbus_message_iter_init_append(signal, &args);
                dbus_uint32_t code = mode == 0 && seq == 1 ? 1 : 0;
                dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &code);
                dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
                if (seq == 0 || (mode == 3 && seq == 2)) {
                    DBusMessageIter entry, variant;
                    const char *key = seq == 0 ? "session_handle" : "streams";
                    const char *value = seq == 0 ? "/test/session" : "wrong-type";
                    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
                    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
                    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
                    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
                    dbus_message_iter_close_container(&entry, &variant);
                    dbus_message_iter_close_container(&dict, &entry);
                }
                dbus_message_iter_close_container(&args, &dict);
            }
            dbus_connection_send(bus, signal, NULL);
            dbus_message_unref(signal);
            dbus_message_unref(call);
            dbus_connection_flush(bus);
        }
    }
    _exit(0);
}

static void test_portal_responses(void) {
    const char *old = getenv("DBUS_SESSION_BUS_ADDRESS");
    char *saved = old ? strdup(old) : NULL;
    char state[] = "/tmp/md-capture-response-XXXXXX";
    MD_CHECK(mkdtemp(state) != NULL);
    old = getenv("XDG_STATE_HOME");
    char *saved_state = old ? strdup(old) : NULL;
    setenv("XDG_STATE_HOME", state, 1);
    for (int mode = 0; mode < 6; mode++) {
        int output[2], ready[2];
        MD_CHECK(pipe(output) == 0 && pipe(ready) == 0);
        pid_t daemon = fork();
        MD_CHECK(daemon >= 0);
        if (daemon == 0) {
            dup2(output[1], STDOUT_FILENO);
            close(output[0]); close(output[1]); close(ready[0]); close(ready[1]);
            execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--print-address=1", (char *)NULL);
            _exit(22);
        }
        close(output[1]);
        FILE *stream = fdopen(output[0], "r");
        char address[512];
        MD_CHECK(stream && fgets(address, sizeof(address), stream));
        address[strcspn(address, "\n")] = 0;
        fclose(stream);
        setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
        pid_t portal = fork();
        MD_CHECK(portal >= 0);
        if (portal == 0) { close(ready[0]); mock_portal(ready[1], mode); }
        close(ready[1]);
        char byte = 0;
        MD_CHECK(read(ready[0], &byte, 1) == 1 && byte == 'R');
        if (mode == 4) MD_CHECK(md_capture_restore_token_save("\xff\xfe") == 0);
        MdCaptureConfig cfg = {.unattended = true, .consent_timeout_ms = 500};
        MdCaptureCtx *ctx = md_capture_create(&cfg);
        MD_CHECK(ctx != NULL);
        double start = test_now();
        MD_CHECK(md_capture_start(ctx) == -1);
        double elapsed = test_now() - start;
        MD_CHECK(elapsed < 1.5);
        if (mode == 2 || mode == 5) MD_CHECK(elapsed >= .4);
        MdFrame frame;
        MD_CHECK(md_capture_get_frame(ctx, &frame, 100) == MD_CAPTURE_ERROR);
        md_capture_destroy(ctx);
        if (mode == 4) {
            char token[16];
            MD_CHECK(md_capture_restore_token_load(token, sizeof(token)) == 2);
            MD_CHECK(md_capture_restore_token_forget() == 0);
        }
        kill(portal, SIGTERM); kill(daemon, SIGTERM);
        MD_CHECK(waitpid(portal, NULL, 0) == portal);
        MD_CHECK(waitpid(daemon, NULL, 0) == daemon);
        char events[16] = {0};
        ssize_t n = read(ready[0], events, sizeof(events)-1);
        MD_CHECK(n > 0);
        close(ready[0]);
        bool start_stall = mode == 2 || mode == 5;
        const char *expected = mode == 0 ? "01X" : mode == 3 ? "012X" : mode == 4 ? "0X" : start_stall ? "012" : "0";
        MD_CHECK(strcmp(events, expected) == 0 || (start_stall && !strcmp(events, "012X")));
    }
    char leaf[512];
    snprintf(leaf, sizeof(leaf), "%s/metadesk", state);
    MD_CHECK(rmdir(leaf) == 0 && rmdir(state) == 0);
    if (saved) { setenv("DBUS_SESSION_BUS_ADDRESS", saved, 1); free(saved); }
    else unsetenv("DBUS_SESSION_BUS_ADDRESS");
    if (saved_state) { setenv("XDG_STATE_HOME", saved_state, 1); free(saved_state); }
    else unsetenv("XDG_STATE_HOME");
    PASS("real backend: denial, malformed response/streams, bounded Start (quiet and chatty bus), corrupt UTF-8 retained");
}

static void test_portal_required(void)
{
    /* A real Linux backend with an unavailable private bus must fail, not
     * connect directly to PipeWire and report a fake successful start. */
    const char *old = getenv("DBUS_SESSION_BUS_ADDRESS");
    char *saved = old ? strdup(old) : NULL;
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/metadesk-capture-test", 1);
    MdCaptureConfig cfg = {.unattended = true, .consent_timeout_ms = 100};
    MdCaptureCtx *ctx = md_capture_create(&cfg);
    MD_CHECK(ctx != NULL);
    for (int i = 0; i < 3; i++) {
        MD_CHECK(md_capture_start(ctx) == -1);
        /* A broken stream answers promptly instead of waiting out a long
         * timeout; there is no size to wait for either. */
        MdFrame frame;
        uint32_t w = 0, h = 0;
        double start = test_now();
        MD_CHECK(md_capture_get_frame(ctx, &frame, 5000) == MD_CAPTURE_ERROR);
        MD_CHECK(md_capture_wait_size(ctx, 5000, &w, &h) == -1 && w == 0 && h == 0);
        MD_CHECK(test_now() - start < 1.0);
        md_capture_stop(ctx);
    }
    md_capture_destroy(ctx);
    if (saved) { setenv("DBUS_SESSION_BUS_ADDRESS", saved, 1); free(saved); }
    else unsetenv("DBUS_SESSION_BUS_ADDRESS");
    PASS("real PipeWire backend fails closed without portal (repeated start/stop)");
}
#endif

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_capture: capture convenience API tests (mock backend)\n");

    test_null_safety();
    test_create_and_destroy();
    test_is_active();
    test_get_size();
    test_get_size_before_init();
    test_display_choose();
    test_output_rect();
    test_wait_size_dispatch();
    test_get_frame_and_release();
    test_config_passthrough();
    test_default_config();
    test_stop_idempotent();
    test_capture_policy();
    test_watchdog();
    test_invalid_cpu_frame();
#ifndef _WIN32
    test_restore_tokens();
#endif
#ifdef __linux__
    test_portal_required();
    test_stalled_bus();
    test_portal_responses();
#endif

    printf("\nAll capture tests passed.\n");
    return md_test_report();
}
