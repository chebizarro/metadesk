/* metadesk — tests/test_capture_dxgi_win.cpp (md-7yc)
 *
 * Standalone Windows harness for the DXGI Desktop Duplication backend against
 * the capture HAL contract (capture.h, md-m0e):
 *
 *   test_capture_dxgi_win.exe                 SSH / session 0: there is no
 *                                             desktop to duplicate, so init
 *                                             must fail cleanly
 *   test_capture_dxgi_win.exe --interactive   console session: frames, IDLE,
 *                                             timeouts, stop, and recovery
 *                                             from duplication loss
 *
 * Frames are read in memory only (never saved) and no input is injected.
 * Duplication loss is simulated through the backend's own loss handler, the
 * code path DXGI_ERROR_ACCESS_LOST takes, rather than by switching the
 * user's desktop. A static desktop presents nothing, and Desktop Duplication
 * delivers nothing at all while the console display is powered off (the VM
 * console's usual state), so frame cells run while the test's own small,
 * non-activating window repaints on a timer. That window also asks for the
 * display to be on (SC_MONITORPOWER to itself, ES_DISPLAY_REQUIRED for its
 * thread's lifetime) and waits for GUID_CONSOLE_DISPLAY_STATE to report it.
 * It never takes focus, sends input or touches other windows.
 */
#include "../src/core/capture_dxgi.cpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    ExitProcess(1); \
} } while (0)

static double now_ms() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* ── Damage source: a 64x64 topmost tool window that changes colour ── */

struct Damage {
    HANDLE ready;
    HANDLE display_on;
    HWND window;
    HANDLE thread;
};

/* GUID_CONSOLE_DISPLAY_STATE; spelled out to avoid needing initguid.h. */
static const GUID console_display_state =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };
static HANDLE damage_display_on = nullptr;

static LRESULT CALLBACK damage_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    static unsigned tick = 0;
    switch (msg) {
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            auto *setting = reinterpret_cast<POWERBROADCAST_SETTING *>(lp);
            DWORD state = 0;
            if (setting->DataLength >= sizeof(state))
                memcpy(&state, setting->Data, sizeof(state));
            if (IsEqualGUID(setting->PowerSetting, console_display_state) && state == 1)
                SetEvent(damage_display_on);
        }
        return TRUE;
    case WM_TIMER:
        tick++;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        HBRUSH brush = CreateSolidBrush(RGB(tick * 37 & 255, tick * 91 & 255, tick * 13 & 255));
        FillRect(dc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

static DWORD WINAPI damage_thread(LPVOID data) {
    auto *d = static_cast<Damage *>(data);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = damage_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MetadeskDxgiDamage";
    CHECK(RegisterClassW(&wc));
    d->window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"md-7yc capture damage", WS_POPUP, 0, 0, 64, 64,
        nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(d->window);
    /* Twice: a scheduled task's STARTUPINFO may carry SW_HIDE. */
    ShowWindow(d->window, SW_SHOWNOACTIVATE);
    ShowWindow(d->window, SW_SHOWNOACTIVATE);
    /* Registration delivers the current display state immediately. */
    HPOWERNOTIFY notify = RegisterPowerSettingNotification(
        d->window, &console_display_state, DEVICE_NOTIFY_WINDOW_HANDLE);
    CHECK(notify);
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    SendMessageW(d->window, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)-1);
    SetTimer(d->window, 1, 30, nullptr);
    SetEvent(d->ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    UnregisterPowerSettingNotification(notify);
    SetThreadExecutionState(ES_CONTINUOUS);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

static void damage_start(Damage *d) {
    d->ready = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    d->display_on = damage_display_on = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    CHECK(d->ready && d->display_on);
    d->thread = CreateThread(nullptr, 0, damage_thread, d, 0, nullptr);
    CHECK(d->thread && WaitForSingleObject(d->ready, 10000) == WAIT_OBJECT_0);
    if (WaitForSingleObject(d->display_on, 10000) != WAIT_OBJECT_0) {
        fprintf(stderr, "FAIL console display did not report on within 10 s; "
                        "Desktop Duplication delivers no frames while it is off\n");
        ExitProcess(1);
    }
}

static void damage_stop(Damage *d) {
    PostMessageW(d->window, WM_CLOSE, 0, 0);
    CHECK(WaitForSingleObject(d->thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(d->thread);
    CloseHandle(d->ready);
    CloseHandle(d->display_on);
    damage_display_on = nullptr;
}

static uint64_t now_ns(const DxgiState *st) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return qpc_to_ns(st, t.QuadPart);
}

/* Validate a delivered frame against the HAL contract and touch every row. */
static void check_frame(const MdCaptureCtx *ctx, const DxgiState *st,
                        const MdFrame &f, uint64_t *last_ts, uint32_t *last_seq) {
    CHECK(f.width == ctx->width && f.height == ctx->height);
    CHECK(f.format == MD_PIX_CAPTURE_BGRA && f.buf_type == MD_BUF_CPU);
    CHECK(f.dmabuf_fd == -1 && !f.gpu_handle && f.data);
    CHECK(f.stride >= f.width * 4 && f.data_size == (size_t)f.stride * f.height);
    unsigned sum = 0;
    for (uint32_t y = 0; y < f.height; y++)
        sum += f.data[(size_t)y * f.stride] + f.data[(size_t)y * f.stride + f.width * 4 - 1];
    (void)sum;
    /* Monotonic nanoseconds on the QPC clock, never in the future. */
    CHECK(f.timestamp_ns <= now_ns(st) && f.timestamp_ns >= *last_ts);
    if (*last_seq != UINT32_MAX) CHECK(f.seq == *last_seq + 1);
    *last_ts = f.timestamp_ns;
    *last_seq = f.seq;
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool interactive = argc > 1 && strcmp(argv[1], "--interactive") == 0;
    const MdCaptureBackend *be = md_capture_backend_create();
    CHECK(be == &dxgi_backend && !be->wait_size && !be->forget_authorization);

    MdCaptureCtx ctx = {};
    ctx.vtable = be;
    ctx.config.target_fps = 60;
    ctx.config.show_cursor = true;
    int rc = be->init(&ctx);
    if (!interactive) {
        CHECK(rc == -1 && !ctx.backend_data && !ctx.active);
        puts("PASS session 0: init fails cleanly without an interactive desktop");
        puts("ALL DXGI CAPTURE TESTS PASSED (session 0)");
        return 0;
    }
    CHECK(rc == 0 && ctx.backend_data);
    auto *st = static_cast<DxgiState *>(ctx.backend_data);
    printf("output %ux%u\n", ctx.width, ctx.height);
    CHECK(ctx.width >= 64 && ctx.height >= 64 && !ctx.active);

    MdFrame f = {};
    double t0 = now_ms();
    CHECK(be->get_frame(&ctx, &f, 2000) == MD_CAPTURE_ERROR && now_ms() - t0 < 50);
    puts("PASS not started: get_frame -> ERROR promptly");

    CHECK(be->start(&ctx) == 0 && ctx.active);
    uint64_t last_ts = 0;
    uint32_t last_seq = UINT32_MAX;

    /* Undamaged: whatever the desktop was doing, an answer within the
     * timeout that is never ERROR. */
    t0 = now_ms();
    rc = be->get_frame(&ctx, &f, 300);
    double took = now_ms() - t0;
    CHECK((rc == 0 || rc == MD_CAPTURE_IDLE) && took < 400);
    if (rc == 0) {
        check_frame(&ctx, st, f, &last_ts, &last_seq);
        be->release_frame(&ctx, &f);
    }
    printf("PASS without test damage: %s after %.0f ms (timeout 300)\n",
           rc == 0 ? "frame" : "IDLE", took);

    Damage damage = {};
    damage_start(&damage);
    CHECK(be->get_frame(&ctx, &f, 2000) == 0);
    check_frame(&ctx, st, f, &last_ts, &last_seq);
    CHECK(st->mapped);
    be->release_frame(&ctx, &f);
    be->release_frame(&ctx, &f); /* idempotent */
    CHECK(!st->mapped);
    puts("PASS damaged desktop delivers a frame (BGRA, CPU, stride, QPC ns, seq)");

    /* A healthy stream answers 0 or IDLE within the timeout, never ERROR. */
    int frames = 0, idles = 0;
    double worst_over = 0;
    for (uint32_t timeout : { 0u, 0u, 100u, 250u, 250u, 500u }) {
        t0 = now_ms();
        rc = be->get_frame(&ctx, &f, timeout);
        double over = now_ms() - t0 - timeout;
        worst_over = over > worst_over ? over : worst_over;
        CHECK(rc == 0 || rc == MD_CAPTURE_IDLE);
        CHECK(over < 100); /* GetTickCount64 granularity plus slack */
        if (rc == 0) {
            frames++;
            check_frame(&ctx, st, f, &last_ts, &last_seq);
            be->release_frame(&ctx, &f);
        } else {
            idles++;
        }
    }
    CHECK(frames > 0);
    printf("PASS healthy stream: %d frames, %d IDLE, worst overrun %.1f ms past timeout\n",
           frames, idles, worst_over);

    /* Duplication loss: ERROR promptly, then the HAL's stop + start recovers
     * and the first frame after re-duplicating is delivered. */
    CHECK(frame_failed(&ctx, st, DXGI_ERROR_ACCESS_LOST) == MD_CAPTURE_ERROR);
    CHECK(!ctx.active && !st->duplication);
    t0 = now_ms();
    CHECK(be->get_frame(&ctx, &f, 2000) == MD_CAPTURE_ERROR && now_ms() - t0 < 50);
    be->stop(&ctx);
    CHECK(be->start(&ctx) == 0 && ctx.active && st->duplication);
    CHECK(be->get_frame(&ctx, &f, 2000) == 0);
    check_frame(&ctx, st, f, &last_ts, &last_seq);
    puts("PASS access loss -> ERROR promptly; stop + start re-duplicates and delivers");

    /* That frame is still borrowed: the next get_frame must reclaim it before
     * copying into the staging texture again. */
    CHECK(st->mapped);
    rc = be->get_frame(&ctx, &f, 100);
    CHECK(rc == 0 || rc == MD_CAPTURE_IDLE);
    if (rc == MD_CAPTURE_IDLE) CHECK(!st->mapped);
    else check_frame(&ctx, st, f, &last_ts, &last_seq);
    be->release_frame(&ctx, &f);
    CHECK(!st->mapped);
    printf("PASS unreleased frame reclaimed by the next get_frame (%s)\n",
           rc == 0 ? "new frame" : "IDLE");

    damage_stop(&damage);

    /* A non-loss failure is reported without tearing the duplication down. */
    CHECK(frame_failed(&ctx, st, E_INVALIDARG) == MD_CAPTURE_ERROR &&
          ctx.active && st->duplication);

    be->stop(&ctx);
    t0 = now_ms();
    CHECK(!ctx.active && be->get_frame(&ctx, &f, 2000) == MD_CAPTURE_ERROR &&
          now_ms() - t0 < 50);
    puts("PASS stopped: get_frame -> ERROR promptly");
    CHECK(be->start(&ctx) == 0 && ctx.active);
    be->stop(&ctx);

    be->destroy(&ctx);
    CHECK(!ctx.backend_data);
    puts("ALL DXGI CAPTURE TESTS PASSED (interactive)");
    return 0;
}
