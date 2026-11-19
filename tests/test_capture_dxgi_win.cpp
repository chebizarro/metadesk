/* metadesk — tests/test_capture_dxgi_win.cpp (md-7yc, md-346)
 *
 * Standalone Windows harness for the DXGI Desktop Duplication backend against
 * the capture HAL contract (capture.h, md-m0e):
 *
 *   test_capture_dxgi_win.exe                 SSH / session 0: pointer
 *                                             compositing and output selection
 *                                             on synthetic data, then init must
 *                                             fail cleanly (no desktop)
 *   test_capture_dxgi_win.exe --interactive   console session: the same, then
 *                                             output selection, frames, IDLE,
 *                                             timeouts, stop, loss recovery,
 *                                             and real pointer shapes
 *
 * Frames are read in memory only (never saved) and no input is injected: the
 * user's pointer is never moved. Duplication loss is simulated through the
 * backend's own loss handler, the code path DXGI_ERROR_ACCESS_LOST takes,
 * rather than by switching the user's desktop. A static desktop presents
 * nothing, and Desktop Duplication delivers nothing at all while the console
 * display is powered off (the VM console's usual state), so frame cells run
 * while the test's own small, non-activating window repaints on a timer.
 * Every test window asks for the display to be on (SC_MONITORPOWER to itself,
 * ES_DISPLAY_REQUIRED for its thread's lifetime) and waits for
 * GUID_CONSOLE_DISPLAY_STATE to report it.
 *
 * Pointer cells show a static, non-activating window around wherever the
 * pointer already is and change only that window's own cursor (SetCursor on
 * its thread), so each change is a pointer-only update. The windows never
 * take focus, send input or touch other windows.
 */
#include "../src/core/capture_dxgi.cpp"

#include <climits>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

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

/* ── Synthetic pointer compositing ───────────────────────────── */

struct Bgr { uint8_t b, g, r; };

struct TestFrame {
    uint32_t w, h, stride;
    std::vector<uint8_t> px;
    TestFrame(uint32_t w_, uint32_t h_, uint32_t pad, Bgr c, uint8_t a)
        : w(w_), h(h_), stride(w_ * 4 + pad), px((size_t)(w_ * 4 + pad) * h_, 0xEE) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint8_t *p = at(x, y);
                p[0] = c.b; p[1] = c.g; p[2] = c.r; p[3] = a;
            }
    }
    uint8_t *at(uint32_t x, uint32_t y) { return px.data() + (size_t)y * stride + (size_t)x * 4; }
};

static bool px_is(const uint8_t *p, Bgr c, int tol = 0) {
    return abs(p[0] - c.b) <= tol && abs(p[1] - c.g) <= tol && abs(p[2] - c.r) <= tol;
}

static bool px_is(Bgr p, Bgr c, int tol = 0) {
    const uint8_t b[3] = { p.b, p.g, p.r };
    return px_is(b, c, tol);
}

static Bgr inverted(Bgr c) { return { (uint8_t)~c.b, (uint8_t)~c.g, (uint8_t)~c.r }; }

static Bgr blended(Bgr s, unsigned a, Bgr d) {
    auto mix = [&](uint8_t sc, uint8_t dc) {
        return (uint8_t)((sc * a + dc * (255u - a) + 127u) / 255u);
    };
    return { mix(s.b, d.b), mix(s.g, d.g), mix(s.r, d.r) };
}

static DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info(UINT type, UINT w, UINT h, UINT pitch) {
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
    si.Type = type;
    si.Width = w;
    si.Height = h;
    si.Pitch = pitch;
    return si;
}

static void draw(TestFrame &f, const DXGI_OUTDUPL_POINTER_SHAPE_INFO &si,
                 const uint8_t *shape, LONG x, LONG y) {
    PointerRect r;
    if (pointer_clip(si, x, y, f.w, f.h, &r)) pointer_blend(f.px.data(), f.stride, r, shape, si, x, y);
}

static void unit_tests() {
    const Bgr bg = { 10, 100, 200 };

    /* Monochrome 4x2: AND row then XOR row per pixel, MSB first. */
    {
        TestFrame f(4, 2, 0, bg, 77);
        const uint8_t mono[4 * 4] = {
            0x30, 0, 0, 0,  0x30, 0, 0, 0,   /* AND: 0 0 1 1 */
            0x50, 0, 0, 0,  0x50, 0, 0, 0,   /* XOR: 0 1 0 1 */
        };
        auto si = shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 4, 4, 4);
        CHECK(pointer_shape_valid(si, sizeof(mono)) && pointer_shape_height(si) == 2);
        draw(f, si, mono, 0, 0);
        for (uint32_t y = 0; y < 2; y++) {
            CHECK(px_is(f.at(0, y), { 0, 0, 0 }));          /* black */
            CHECK(px_is(f.at(1, y), { 255, 255, 255 }));    /* white */
            CHECK(px_is(f.at(2, y), bg));                   /* transparent */
            CHECK(px_is(f.at(3, y), inverted(bg)));         /* invert */
            for (uint32_t x = 0; x < 4; x++) CHECK(f.at(x, y)[3] == 77);
        }
    }
    /* Color: premultiplied alpha 0, 255 and 128; colour above alpha clamps. */
    {
        TestFrame f(4, 1, 0, bg, 255);
        const uint8_t color[4 * 4] = {
            0, 0, 0, 0,  40, 50, 60, 255,  100, 0, 128, 128,  255, 255, 255, 128 };
        auto si = shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 4, 1, 16);
        CHECK(pointer_shape_valid(si, sizeof(color)));
        draw(f, si, color, 0, 0);
        CHECK(px_is(f.at(0, 0), bg));
        CHECK(px_is(f.at(1, 0), { 40, 50, 60 }));
        /* 50% of straight (200, 0, 255) premultiplied, over bg. */
        CHECK(px_is(f.at(2, 0), { 105, 50, 228 }));
        CHECK(px_is(f.at(2, 0), blended({ 200, 0, 255 }, 128, bg), 1));
        CHECK(px_is(f.at(3, 0), { 255, 255, 255 }));
    }
    /* Masked color: alpha 0 replaces, 0xFF XORs. */
    {
        TestFrame f(2, 1, 0, bg, 255);
        const uint8_t masked[2 * 4] = { 7, 8, 9, 0,  0xFF, 0x0F, 0, 0xFF };
        auto si = shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, 2, 1, 8);
        CHECK(pointer_shape_valid(si, sizeof(masked)));
        draw(f, si, masked, 0, 0);
        CHECK(px_is(f.at(0, 0), { 7, 8, 9 }));
        CHECK(px_is(f.at(1, 0), { (uint8_t)(bg.b ^ 0xFF), (uint8_t)(bg.g ^ 0x0F), bg.r }));
    }
    puts("PASS pointer shapes: monochrome AND/XOR, color alpha, masked-color replace/XOR");

    /* Clipping at every edge never writes outside the frame or its padding. */
    {
        std::vector<uint8_t> opaque(4 * 4 * 4);
        for (size_t i = 0; i < opaque.size(); i += 4) {
            opaque[i] = 0xAA; opaque[i + 1] = 0xBB; opaque[i + 2] = 0xCC; opaque[i + 3] = 0xFF;
        }
        auto si = shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 4, 4, 16);
        TestFrame f(8, 8, 8, bg, 255);
        draw(f, si, opaque.data(), -2, -2);
        draw(f, si, opaque.data(), 6, 6);
        draw(f, si, opaque.data(), 6, -3);
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++) {
                const bool hit = (x < 2 && y < 2) || (x >= 6 && y >= 6) || (x >= 6 && y < 1);
                CHECK(px_is(f.at(x, y), hit ? Bgr{ 0xAA, 0xBB, 0xCC } : bg));
            }
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t i = 0; i < 8; i++) CHECK(f.px[(size_t)y * f.stride + 32 + i] == 0xEE);
        PointerRect r;
        CHECK(!pointer_clip(si, 8, 0, 8, 8, &r) && !pointer_clip(si, 0, -4, 8, 8, &r));
        CHECK(!pointer_clip(si, LONG_MIN, LONG_MIN, 8, 8, &r));
        CHECK(!pointer_clip(si, LONG_MAX, 0, 8, 8, &r));
    }
    /* Shapes that do not fit their buffer are rejected. */
    {
        CHECK(!pointer_shape_valid(shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 4, 4, 15), 1024));
        CHECK(!pointer_shape_valid(shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 4, 4, 16), 63));
        CHECK(pointer_shape_valid(shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 4, 4, 32), 112));
        CHECK(!pointer_shape_valid(shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 8, 3, 1), 64));
        CHECK(!pointer_shape_valid(shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 9, 2, 1), 64));
        CHECK(!pointer_shape_valid(shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, 0, 4, 16), 64));
        CHECK(!pointer_shape_valid(shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 4, 0, 16), 64));
        CHECK(!pointer_shape_valid(shape_info(8u, 4, 4, 16), 64));
    }
    puts("PASS pointer clipping at all edges; oversized or malformed shapes rejected");

    /* Composite, move, hide: the saved pixels restore the frame exactly. */
    {
        TestFrame f(16, 16, 4, bg, 255);
        const std::vector<uint8_t> original = f.px;
        uint8_t shape[3 * 3 * 4];
        for (size_t i = 0; i < sizeof(shape); i += 4) {
            shape[i] = 1; shape[i + 1] = 2; shape[i + 2] = 3; shape[i + 3] = 0xFF;
        }
        PointerState p = {};
        p.visible = true;
        p.has_shape = true;
        p.shape = shape;
        p.shape_info = shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 3, 3, 12);
        p.x = 2; p.y = 3;
        pointer_composite(&p, f.px.data(), f.stride, f.w, f.h);
        CHECK(px_is(f.at(2, 3), { 1, 2, 3 }) && px_is(f.at(4, 5), { 1, 2, 3 }));
        p.x = 14; p.y = -1;                   /* move, partly off-frame */
        pointer_composite(&p, f.px.data(), f.stride, f.w, f.h);
        CHECK(px_is(f.at(2, 3), bg) && px_is(f.at(15, 1), { 1, 2, 3 }));
        p.visible = false;
        pointer_composite(&p, f.px.data(), f.stride, f.w, f.h);
        CHECK(f.px == original && p.under_rect.w == 0);
        free(p.under);
    }
    puts("PASS save-under: move and hide restore the uncomposited pixels exactly");

    /* Which pointer updates are damage (on a 100x100 frame). */
    {
        PointerState p = {};
        uint8_t shape[4 * 4 * 4] = {};
        p.shape = shape;
        p.shape_info = shape_info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 4, 4, 16);
        auto apply = [&](const DXGI_OUTDUPL_FRAME_INFO &i, bool new_shape) {
            PointerRect r;
            const bool drawn = pointer_drawn(&p, 100, 100, &r);
            return pointer_apply(&p, i, drawn, new_shape, 100, 100);
        };
        DXGI_OUTDUPL_FRAME_INFO info = {};
        CHECK(!apply(info, false));                           /* no update */
        info.LastMouseUpdateTime.QuadPart = 1;
        info.PointerPosition.Visible = TRUE;
        info.PointerPosition.Position = { 5, 6 };
        CHECK(!apply(info, false) && p.visible);              /* shown, no shape yet */
        p.has_shape = true;
        CHECK(apply(info, true));                             /* shape arrives */
        CHECK(!apply(info, false));                           /* same place */
        info.PointerPosition.Position = { 7, 6 };
        CHECK(apply(info, false) && p.x == 7);                /* moved */
        CHECK(apply(info, true));                             /* new shape */
        DXGI_OUTDUPL_FRAME_INFO stale = info;
        stale.LastMouseUpdateTime.QuadPart = 0;
        stale.PointerPosition.Position = { 99, 99 };
        CHECK(!apply(stale, false) && p.x == 7);              /* position is stale */
        info.PointerPosition.Position = { 200, 6 };
        CHECK(apply(info, false));                            /* moved off the frame */
        info.PointerPosition.Position = { 300, 6 };
        CHECK(!apply(info, false));                           /* moved while off it */
        info.PointerPosition.Position = { 7, 6 };
        CHECK(apply(info, false));                            /* back on it */
        info.PointerPosition.Visible = FALSE;
        CHECK(apply(info, false) && !p.visible);              /* hidden */
        info.PointerPosition.Position = { 50, 60 };
        CHECK(!apply(info, false));                           /* moved while hidden */
        CHECK(!apply(info, true));                            /* new shape, hidden */
        info.PointerPosition.Visible = TRUE;
        CHECK(apply(info, false));                            /* shown again */
        PointerRect r;
        const bool was_drawn = pointer_drawn(&p, 100, 100, &r);
        p.has_shape = false;                                  /* unusable new shape: */
        CHECK(was_drawn && pointer_apply(&p, info, was_drawn, true, 100, 100)); /* removed */
    }
    puts("PASS pointer damage: appear, disappear, move or new shape while drawn; else none");

    /* Output selectors. */
    {
        DXGI_OUTPUT_DESC d[2] = {};
        wcscpy_s(d[0].DeviceName, _countof(d[0].DeviceName), L"\\\\.\\DISPLAY1");
        d[0].DesktopCoordinates = { 1920, 0, 3840, 1080 };
        wcscpy_s(d[1].DeviceName, _countof(d[1].DeviceName), L"\\\\.\\DISPLAY2");
        d[1].DesktopCoordinates = { 0, 0, 1920, 1080 };
        CHECK(output_choose(nullptr, d, 2) == 1 && output_choose("", d, 2) == 1);
        CHECK(output_choose("0", d, 2) == 0 && output_choose("1", d, 2) == 1);
        CHECK(output_choose("01", d, 2) == 1);
        CHECK(output_choose("2", d, 2) == -1 && output_choose("99999", d, 2) == -1);
        CHECK(output_choose("display1", d, 2) == 0);
        CHECK(output_choose("\\\\.\\DISPLAY2", d, 2) == 1);
        CHECK(output_choose("\\\\.\\display2", d, 2) == 1);
        CHECK(output_choose("DISPLAY3", d, 2) == -1 && output_choose("-1", d, 2) == -1);
        CHECK(output_choose("id:2", d, 2) == 1 && output_choose("ID:1", d, 2) == 0);
        CHECK(output_choose("id:3", d, 2) == -1 && output_choose("id:0", d, 2) == -1);
        CHECK(display_number("DISPLAY12") == 12 && display_number("display7") == 7);
        CHECK(display_number("DISPLAY") == 0 && display_number("DISPLAY2x") == 0 &&
              display_number("MONITOR2") == 0);
        CHECK(output_choose("1x", d, 2) == -1 && output_choose(" 1", d, 2) == -1);
        d[1].DesktopCoordinates = { -1920, 0, 0, 1080 };      /* no output at origin */
        CHECK(output_choose(nullptr, d, 2) == 0);
        CHECK(output_choose(nullptr, d, 0) == -1 && output_choose("0", d, 0) == -1);
    }
    puts("PASS output selectors: primary default, index, device name; unknown -> none");
}

/* ── Test windows ────────────────────────────────────────────── */

/* GUID_CONSOLE_DISPLAY_STATE; spelled out to avoid needing initguid.h. */
static const GUID console_display_state =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };
static const UINT WM_SET_TEST_CURSOR = WM_APP + 1;
static const wchar_t test_class[] = L"MetadeskDxgiTest";

struct TestWindow {
    RECT rect;        /* virtual-desktop coordinates */
    bool animate;     /* repaint on a timer (damage) or stay static */
    COLORREF color;   /* static fill */
    HCURSOR cursor;   /* client-area cursor; owned by the window thread */
    unsigned tick;
    HANDLE ready, display_on, thread;
    HWND window;
};

static LRESULT CALLBACK test_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto *tw = reinterpret_cast<TestWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)
                          reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        break;
    case WM_POWERBROADCAST:
        if (tw && wp == PBT_POWERSETTINGCHANGE) {
            auto *setting = reinterpret_cast<POWERBROADCAST_SETTING *>(lp);
            DWORD state = 0;
            if (setting->DataLength >= sizeof(state))
                memcpy(&state, setting->Data, sizeof(state));
            if (IsEqualGUID(setting->PowerSetting, console_display_state)) {
                /* The VM console turns the display off again within about
                 * a second of being woken, ES_DISPLAY_REQUIRED or not, and
                 * duplication then stalls: wake it again each time. */
                if (state == 1) SetEvent(tw->display_on);
                else PostMessageW(window, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)-1);
            }
        }
        return TRUE;
    case WM_TIMER:
        tw->tick++;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (tw && LOWORD(lp) == HTCLIENT) {
            SetCursor(tw->cursor);
            return TRUE;
        }
        break;
    case WM_SET_TEST_CURSOR:
        tw->cursor = (HCURSOR)wp;
        SetCursor(tw->cursor); /* the pointer is over this thread's window */
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        const unsigned t = tw ? tw->tick : 0;
        HBRUSH brush = CreateSolidBrush(tw && !tw->animate ? tw->color
            : RGB(t * 37 & 255, t * 91 & 255, t * 13 & 255));
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

static DWORD WINAPI test_window_thread(LPVOID data) {
    auto *tw = static_cast<TestWindow *>(data);
    const RECT &r = tw->rect;
    tw->window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        test_class, L"metadesk DXGI capture test", WS_POPUP, r.left, r.top,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), tw);
    CHECK(tw->window);
    /* Twice: a scheduled task's STARTUPINFO may carry SW_HIDE. */
    ShowWindow(tw->window, SW_SHOWNOACTIVATE);
    ShowWindow(tw->window, SW_SHOWNOACTIVATE);
    UpdateWindow(tw->window);
    /* Registration delivers the current display state immediately. */
    HPOWERNOTIFY notify = RegisterPowerSettingNotification(
        tw->window, &console_display_state, DEVICE_NOTIFY_WINDOW_HANDLE);
    CHECK(notify);
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    SendMessageW(tw->window, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)-1);
    if (tw->animate) SetTimer(tw->window, 1, 30, nullptr);
    SetEvent(tw->ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    UnregisterPowerSettingNotification(notify);
    SetThreadExecutionState(ES_CONTINUOUS);
    return 0;
}

static void window_start(TestWindow *tw) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = test_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = test_class;
        CHECK(RegisterClassW(&wc));
        registered = true;
    }
    tw->ready = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    tw->display_on = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    CHECK(tw->ready && tw->display_on);
    tw->thread = CreateThread(nullptr, 0, test_window_thread, tw, 0, nullptr);
    CHECK(tw->thread && WaitForSingleObject(tw->ready, 10000) == WAIT_OBJECT_0);
    if (WaitForSingleObject(tw->display_on, 10000) != WAIT_OBJECT_0) {
        fprintf(stderr, "FAIL console display did not report on within 10 s; "
                        "Desktop Duplication delivers no frames while it is off\n");
        ExitProcess(1);
    }
}

static void window_stop(TestWindow *tw) {
    PostMessageW(tw->window, WM_CLOSE, 0, 0);
    CHECK(WaitForSingleObject(tw->thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(tw->thread);
    CloseHandle(tw->ready);
    CloseHandle(tw->display_on);
}

/* ── Frame helpers ───────────────────────────────────────────── */

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

/* Consume frames until the desktop has been still for `quiet_ms`. */
static bool drain_to_idle(const MdCaptureBackend *be, MdCaptureCtx *ctx, uint32_t quiet_ms,
                          double budget_ms) {
    const double end = now_ms() + budget_ms;
    MdFrame f = {};
    while (now_ms() < end) {
        const int rc = be->get_frame(ctx, &f, quiet_ms);
        CHECK(rc == 0 || rc == MD_CAPTURE_IDLE);
        if (rc == MD_CAPTURE_IDLE) return true;
        be->release_frame(ctx, &f);
    }
    return false;
}

static MdCaptureCtx make_ctx(const MdCaptureBackend *be, bool show_cursor, const char *output) {
    MdCaptureCtx ctx = {};
    ctx.vtable = be;
    ctx.config.target_fps = 60;
    ctx.config.show_cursor = show_cursor;
    ctx.config.output = output;
    return ctx;
}

/* ── Interactive: output selection ───────────────────────────── */

static void output_cells(const MdCaptureBackend *be) {
    OutputList outs = {};
    CHECK(SUCCEEDED(outputs_enum(&outs)) && outs.count > 0);
    int primary = -1;
    for (UINT i = 0; i < outs.count; i++) {
        char name[128];
        output_name(outs.desc[i], name, (int)sizeof(name));
        const RECT &r = outs.desc[i].DesktopCoordinates, &p = outs.rect[i];
        printf("output %u: %s %ldx%ld at (%ld,%ld)%s; physical %ldx%ld at (%ld,%ld)\n", i, name,
               r.right - r.left, r.bottom - r.top, r.left, r.top,
               output_is_primary(outs.desc[i]) ? " primary" : "", p.right - p.left,
               p.bottom - p.top, p.left, p.top);
        if (primary < 0 && output_is_primary(outs.desc[i])) primary = (int)i;
    }
    CHECK(primary >= 0);

    MdCaptureCtx ctx = make_ctx(be, true, nullptr);
    CHECK(be->init(&ctx) == 0);
    auto *st = static_cast<DxgiState *>(ctx.backend_data);
    CHECK(st->output_index == (UINT)primary && output_is_primary(st->output_desc));
    be->destroy(&ctx);
    puts("PASS default output is the primary display");

    char index[16], name[128];
    for (UINT i = 0; i < outs.count; i++) {
        snprintf(index, sizeof(index), "%u", i);
        output_name(outs.desc[i], name, (int)sizeof(name));
        for (const char *spec : { (const char *)index, (const char *)name,
                                  strip_device_prefix(name) }) {
            ctx = make_ctx(be, true, spec);
            CHECK(be->init(&ctx) == 0);
            st = static_cast<DxgiState *>(ctx.backend_data);
            CHECK(st->output_index == i);
            const RECT &r = outs.rect[i];
            const uint32_t dw = (uint32_t)(r.right - r.left), dh = (uint32_t)(r.bottom - r.top);
            CHECK((ctx.width == dw && ctx.height == dh) || (ctx.width == dh && ctx.height == dw));
            /* md-w65: where input must aim, in physical desktop pixels. */
            CHECK(ctx.output_rect.x == r.left && ctx.output_rect.y == r.top &&
                  ctx.output_rect.width == dw && ctx.output_rect.height == dh);
            be->destroy(&ctx);
        }
    }
    printf("PASS every output selectable by index, device name and bare name (%u); "
           "ctx->output_rect is its physical rectangle\n", outs.count);

    snprintf(index, sizeof(index), "%u", outs.count);
    for (const char *spec : { (const char *)index, "DISPLAY99", "1x" }) {
        ctx = make_ctx(be, true, spec);
        CHECK(be->init(&ctx) == -1 && !ctx.backend_data);
    }
    outputs_release(&outs);
    puts("PASS unknown outputs fail init (attached outputs logged above)");
}

/* ── Interactive: real pointer shapes ────────────────────────── */

enum { QUAD = 4 };

struct TestCursor {
    const char *label;
    UINT type;               /* expected DXGI shape type */
    POINT hotspot;
    HCURSOR cursor;
    Bgr expect[QUAD];        /* TL, TR, BL, BR quadrant centres */
    int tol;
};

static const int CUR = 32;

/* Monochrome: TL black, TR white, BL transparent, BR invert. */
static HCURSOR make_mono_cursor(POINT hot) {
    uint8_t and_plane[CUR * CUR / 8], xor_plane[CUR * CUR / 8];
    for (int y = 0; y < CUR; y++)
        for (int b = 0; b < CUR / 8; b++) {
            const bool right = b >= CUR / 16, bottom = y >= CUR / 2;
            and_plane[y * CUR / 8 + b] = bottom ? 0xFF : 0x00;
            xor_plane[y * CUR / 8 + b] = right ? 0xFF : 0x00;
        }
    return CreateCursor(GetModuleHandleW(nullptr), hot.x, hot.y, CUR, CUR, and_plane, xor_plane);
}

/* 32bpp cursor from per-quadrant BGRA pixels and mask bits. */
static HCURSOR make_color_cursor(POINT hot, const uint32_t quad_bgra[QUAD], const bool quad_mask[QUAD]) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = CUR;
    bi.bmiHeader.biHeight = -CUR; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    CHECK(color && bits);
    uint8_t mask_bits[CUR * CUR / 8] = {};
    for (int y = 0; y < CUR; y++)
        for (int x = 0; x < CUR; x++) {
            const int q = (y >= CUR / 2) * 2 + (x >= CUR / 2);
            static_cast<uint32_t *>(bits)[y * CUR + x] = quad_bgra[q];
            if (quad_mask[q]) mask_bits[y * CUR / 8 + x / 8] |= (uint8_t)(0x80 >> (x % 8));
        }
    HBITMAP mask = CreateBitmap(CUR, CUR, 1, 1, mask_bits);
    CHECK(mask);
    ICONINFO ii = {};
    ii.fIcon = FALSE;
    ii.xHotspot = (DWORD)hot.x;
    ii.yHotspot = (DWORD)hot.y;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HCURSOR cursor = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return cursor;
}

static Bgr frame_px(const MdFrame &f, LONG x, LONG y) {
    const uint8_t *p = f.data + (size_t)y * f.stride + (size_t)x * 4;
    return { p[0], p[1], p[2] };
}

/* Where a test cursor's quadrant centres land, in frame coordinates. */
struct Samples {
    LONG x[QUAD], y[QUAD];
    bool in[QUAD];   /* on the frame and over the test window */
    int count;
};

static Samples samples(LONG left, LONG top, const RECT &win, const MdFrame &f) {
    Samples s = {};
    for (int q = 0; q < QUAD; q++) {
        s.x[q] = left + CUR / 4 + (q % 2) * CUR / 2;
        s.y[q] = top + CUR / 4 + (q / 2) * CUR / 2;
        s.in[q] = s.x[q] >= win.left && s.x[q] < win.right && s.y[q] >= win.top &&
                  s.y[q] < win.bottom && s.x[q] >= 0 && s.y[q] >= 0 &&
                  s.x[q] < (LONG)f.width && s.y[q] < (LONG)f.height;
        s.count += s.in[q] ? 1 : 0;
    }
    return s;
}

static bool samples_match(const MdFrame &f, const Samples &s, const Bgr expect[QUAD], int tol) {
    for (int q = 0; q < QUAD; q++)
        if (s.in[q] && !px_is(frame_px(f, s.x[q], s.y[q]), expect[q], tol)) return false;
    return s.count > 0;
}

static void samples_dump(const char *label, const MdFrame &f, const Samples &s,
                         const Bgr expect[QUAD]) {
    for (int q = 0; q < QUAD; q++) {
        if (!s.in[q]) continue;
        const Bgr got = frame_px(f, s.x[q], s.y[q]);
        fprintf(stderr, "  %s quadrant %d at (%ld,%ld): got %u,%u,%u want %u,%u,%u\n", label, q,
                s.x[q], s.y[q], got.b, got.g, got.r, expect[q].b, expect[q].g, expect[q].r);
    }
}

/* Get frames for up to 3 s until the cursor quadrants show `expect`. Returns
 * the still-borrowed frame, and whether the first answer was a frame. */
static bool wait_pixels(const MdCaptureBackend *be, MdCaptureCtx *ctx, LONG left, LONG top,
                        const RECT &win, const Bgr expect[QUAD], int tol, MdFrame *f,
                        bool *first_was_frame) {
    const double end = now_ms() + 3000;
    *first_was_frame = false;
    int misses = 0;
    for (bool first = true; now_ms() < end; first = false) {
        const int rc = be->get_frame(ctx, f, 1000);
        CHECK(rc == 0 || rc == MD_CAPTURE_IDLE);
        if (rc != 0) continue;
        if (first) *first_was_frame = true;
        const Samples s = samples(left, top, win, *f);
        if (samples_match(*f, s, expect, tol)) return true;
        if (misses++ < 3) { /* transient misses are normal; these explain a failure */
            const PointerState &p = static_cast<DxgiState *>(ctx->backend_data)->pointer;
            fprintf(stderr, "  no match in frame %u (DXGI pointer visible %d at %ld,%ld)\n",
                    f->seq, p.visible, p.x, p.y);
            samples_dump("   ", *f, s, expect);
        }
        be->release_frame(ctx, f);
    }
    return false;
}

enum class Seen { none, composited, software };

/* Get frames for up to 3 s until cursor c shows up, either composited by the
 * backend from DXGI's shape (which must then match exactly) or, when the
 * driver cannot show it as a hardware pointer, drawn into the desktop image
 * by DWM with DXGI reporting no pointer (the backend must draw nothing). */
static Seen wait_cursor(const MdCaptureBackend *be, MdCaptureCtx *ctx, const TestCursor &c,
                        LONG left, LONG top, const RECT &win, Bgr bg, MdFrame *f,
                        bool *first_was_frame, bool *exact) {
    const PointerState &p = static_cast<DxgiState *>(ctx->backend_data)->pointer;
    const double end = now_ms() + 3000;
    *first_was_frame = false;
    int misses = 0;
    for (bool first = true; now_ms() < end; first = false) {
        const int rc = be->get_frame(ctx, f, 1000);
        CHECK(rc == 0 || rc == MD_CAPTURE_IDLE);
        if (rc != 0) continue;
        if (first) *first_was_frame = true;
        const Samples s = samples(left, top, win, *f);
        *exact = samples_match(*f, s, c.expect, c.tol);
        if (p.visible && p.has_shape && p.shape_info.Type == c.type &&
            p.shape_info.HotSpot.x == c.hotspot.x && p.shape_info.HotSpot.y == c.hotspot.y &&
            *exact)
            return Seen::composited;
        if (!p.visible && p.under_rect.w == 0) {
            for (int q = 0; q < QUAD; q++)
                if (s.in[q] && !px_is(frame_px(*f, s.x[q], s.y[q]), bg)) return Seen::software;
        }
        if (misses++ < 3) { /* transient misses are normal; these explain a failure */
            fprintf(stderr, "  %s: no match in frame %u (DXGI pointer visible %d at %ld,%ld)\n",
                    c.label, f->seq, p.visible, p.x, p.y);
            samples_dump("   ", *f, s, c.expect);
        }
        be->release_frame(ctx, f);
    }
    return Seen::none;
}

static void pointer_cells(const MdCaptureBackend *be, MdCaptureCtx *ctx) {
    auto *st = static_cast<DxgiState *>(ctx->backend_data);
    const RECT out = st->output_desc.DesktopCoordinates;
    POINT pt;
    CHECK(GetCursorPos(&pt));
    if (pt.x < out.left || pt.x >= out.right || pt.y < out.top || pt.y >= out.bottom) {
        printf("SKIP pointer cells: the pointer (%ld,%ld) is not on the captured output\n",
               pt.x, pt.y);
        return;
    }
    const POINT at = { pt.x - out.left, pt.y - out.top }; /* frame coordinates */
    /* A static window around the pointer, inside the output. */
    const LONG half = 120;
    TestWindow tw = {};
    tw.color = RGB(200, 100, 10);
    const Bgr bg = { 10, 100, 200 };
    LONG left = pt.x - half, top = pt.y - half;
    left = left < out.left ? out.left : left > out.right - 2 * half ? out.right - 2 * half : left;
    top = top < out.top ? out.top : top > out.bottom - 2 * half ? out.bottom - 2 * half : top;
    tw.rect = { left, top, left + 2 * half, top + 2 * half };
    const RECT win = { left - out.left, top - out.top, left - out.left + 2 * half,
                       top - out.top + 2 * half };

    const uint32_t red = 0xFFFF0000, clear = 0x00000000, semi = 0x80FF0000, blue = 0xFF0000FF;
    const uint32_t color_quads[QUAD] = { red, clear, semi, blue };
    const bool no_mask[QUAD] = { false, false, false, false };
    const uint32_t green = 0x0000FF00, black = 0x00000000, white = 0x00FFFFFF, blue0 = 0x000000FF;
    const uint32_t masked_quads[QUAD] = { green, black, white, blue0 };
    const bool masked_mask[QUAD] = { false, true, true, false };
    TestCursor cursors[] = {
        { "monochrome", DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, { 5, 7 }, nullptr,
          { { 0, 0, 0 }, { 255, 255, 255 }, bg, inverted(bg) }, 0 },
        { "color", DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, { 3, 4 }, nullptr,
          { { 0, 0, 255 }, bg, blended({ 0, 0, 255 }, 0x80, bg), { 255, 0, 0 } }, 2 },
        { "masked color", DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, { 9, 2 }, nullptr,
          { { 0, 255, 0 }, bg, inverted(bg), { 255, 0, 0 } }, 0 },
    };
    cursors[0].cursor = make_mono_cursor(cursors[0].hotspot);
    cursors[1].cursor = make_color_cursor(cursors[1].hotspot, color_quads, no_mask);
    cursors[2].cursor = make_color_cursor(cursors[2].hotspot, masked_quads, masked_mask);
    for (const TestCursor &c : cursors) CHECK(c.cursor);
    const HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
    tw.cursor = arrow; /* differs from every test shape */

    window_start(&tw);
    MdFrame f = {};
    bool moved = false;
    int pointer_only = 0;
    const TestCursor *hardware = nullptr; /* a shape DXGI reports as a pointer */
    for (const TestCursor &c : cursors) {
        const bool idle = drain_to_idle(be, ctx, 300, 3000);
        SendMessageW(tw.window, WM_SET_TEST_CURSOR, (WPARAM)c.cursor, 0);
        const LONG sx = at.x - c.hotspot.x, sy = at.y - c.hotspot.y;
        bool first = false, exact = false;
        const Seen seen = wait_cursor(be, ctx, c, sx, sy, win, bg, &f, &first, &exact);
        if (seen == Seen::none) {
            const PointerState &p = st->pointer;
            fprintf(stderr, "FAIL %s pointer never appeared in a frame (DXGI pointer: visible "
                    "%d at %ld,%ld, shape %d type %u hotspot %ld,%ld)\n", c.label, p.visible,
                    p.x, p.y, p.has_shape, p.shape_info.Type, p.shape_info.HotSpot.x,
                    p.shape_info.HotSpot.y);
            ExitProcess(1);
        }
        POINT now;
        CHECK(GetCursorPos(&now));
        moved = moved || now.x != pt.x || now.y != pt.y;
        const PointerState &p = st->pointer;
        if (seen == Seen::composited) {
            /* Position is the shape's top-left: Position + HotSpot is the
             * pointer, and the pixels matched at pointer - hotspot. */
            CHECK(p.shape_info.Width == (UINT)CUR && pointer_shape_height(p.shape_info) == (UINT)CUR);
            if (!moved) CHECK(p.x == sx && p.y == sy);
            if (idle && first) pointer_only++;
            hardware = hardware ? hardware : &c;
        }
        char raw[64] = "";
        if (p.visible && c.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
            /* Evidence for the premultiplied blend: the 50% red quadrant. */
            const uint8_t *s = p.shape + (size_t)(CUR * 3 / 4) * p.shape_info.Pitch + CUR / 4 * 4;
            snprintf(raw, sizeof(raw), "; 50%% red shape pixel BGRA %u,%u,%u,%u",
                     s[0], s[1], s[2], s[3]);
        }
        printf("PASS %s pointer in the frame at %ld,%ld (hotspot %ld,%ld): %s%s%s\n", c.label,
               sx, sy, c.hotspot.x, c.hotspot.y,
               seen == Seen::composited ? "composited from the DXGI shape"
               : exact ? "software cursor: DWM drew it into the desktop image and DXGI "
                         "reports no pointer, so the backend drew nothing"
                       : "software cursor: DWM drew it into the desktop image (its own "
                         "rendering) and DXGI reports no pointer, so the backend drew nothing",
               p.visible && idle && first ? "; pointer-only update delivered" : "", raw);
        be->release_frame(ctx, &f);
    }
    if (moved) puts("note: the user moved the pointer; positions were not compared");
    CHECK(hardware); /* color shapes are hardware pointers on every driver seen */
    CHECK(pointer_only > 0);

    /* Hide: the pixels under the last pointer go back to the window colour. */
    {
        const TestCursor &c = cursors[2];
        const LONG sx = at.x - c.hotspot.x, sy = at.y - c.hotspot.y;
        const Bgr plain[QUAD] = { bg, bg, bg, bg };
        drain_to_idle(be, ctx, 300, 3000);
        SendMessageW(tw.window, WM_SET_TEST_CURSOR, 0, 0);
        bool first = false;
        CHECK(wait_pixels(be, ctx, sx, sy, win, plain, 0, &f, &first));
        CHECK(!st->pointer.visible);
        be->release_frame(ctx, &f);
        puts("PASS hidden pointer: frame delivered, pixels under it restored");
    }
    /* Hide a composited pointer: removed by a pointer-only update. */
    {
        const TestCursor &c = *hardware;
        const LONG sx = at.x - c.hotspot.x, sy = at.y - c.hotspot.y;
        const Bgr plain[QUAD] = { bg, bg, bg, bg };
        bool first = false;
        SendMessageW(tw.window, WM_SET_TEST_CURSOR, (WPARAM)c.cursor, 0);
        CHECK(wait_pixels(be, ctx, sx, sy, win, c.expect, c.tol, &f, &first));
        be->release_frame(ctx, &f);
        const bool idle = drain_to_idle(be, ctx, 300, 3000);
        SendMessageW(tw.window, WM_SET_TEST_CURSOR, 0, 0);
        CHECK(wait_pixels(be, ctx, sx, sy, win, plain, 0, &f, &first));
        CHECK(!st->pointer.visible);
        be->release_frame(ctx, &f);
        printf("PASS hidden %s pointer removed from the composited frame%s\n", c.label,
               idle && first ? " by a pointer-only update" : "");
    }
    printf("PASS pointer-only shape changes delivered as frames: %d\n", pointer_only);

    /* show_cursor off: the same pointer-only updates are consumed as no
     * damage. The pointer update arrives within milliseconds of the change,
     * but the VM desktop is never quiet for long (a full redraw after each
     * display wake, the taskbar, the task's console caret blinking every
     * ~530 ms), so wait briefly and count an attempt only when the call
     * returned IDLE and the backend consumed an update meanwhile. */
    MdCaptureConfig saved = ctx->config;
    be->destroy(ctx);
    *ctx = make_ctx(be, false, saved.output);
    CHECK(be->init(ctx) == 0 && be->start(ctx) == 0);
    st = static_cast<DxgiState *>(ctx->backend_data);
    int idle_cells = 0;
    for (int attempt = 0; attempt < 12 && idle_cells < 2; attempt++) {
        if (!drain_to_idle(be, ctx, 300, 3000)) continue;
        SendMessageW(tw.window, WM_SET_TEST_CURSOR,
                     (WPARAM)(attempt % 2 ? arrow : hardware->cursor), 0);
        const uint32_t skipped = st->skipped;
        const int rc = be->get_frame(ctx, &f, 150);
        CHECK(rc == 0 || rc == MD_CAPTURE_IDLE);
        if (rc == 0) be->release_frame(ctx, &f); /* unrelated desktop damage */
        else if (st->skipped > skipped) idle_cells++;
    }
    CHECK(idle_cells == 2);
    puts("PASS show_cursor off: pointer-only updates stay IDLE");

    window_stop(&tw);
    for (const TestCursor &c : cursors) DestroyCursor(c.cursor);
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool interactive = argc > 1 && strcmp(argv[1], "--interactive") == 0;
    unit_tests();

    const MdCaptureBackend *be = md_capture_backend_create();
    CHECK(be == &dxgi_backend && !be->wait_size && !be->forget_authorization);

    MdCaptureCtx ctx = make_ctx(be, true, nullptr);
    int rc = be->init(&ctx);
    if (!interactive) {
        CHECK(rc == -1 && !ctx.backend_data && !ctx.active);
        ctx = make_ctx(be, true, "99");
        CHECK(be->init(&ctx) == -1 && !ctx.backend_data);
        puts("PASS session 0: init fails cleanly without an interactive desktop");
        puts("ALL DXGI CAPTURE TESTS PASSED (session 0)");
        return 0;
    }
    CHECK(rc == 0 && ctx.backend_data);
    be->destroy(&ctx);
    output_cells(be);

    ctx = make_ctx(be, true, nullptr);
    CHECK(be->init(&ctx) == 0);
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

    const RECT origin = st->output_desc.DesktopCoordinates;
    TestWindow damage = {};
    damage.rect = { origin.left, origin.top, origin.left + 64, origin.top + 64 };
    damage.animate = true;
    window_start(&damage);
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
        if (over >= 100)
            fprintf(stderr, "timeout %u: %s after %.1f ms\n", timeout,
                    rc == 0 ? "frame" : "IDLE", over + timeout);
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

    window_stop(&damage);

    /* A non-loss failure is reported without tearing the duplication down. */
    CHECK(frame_failed(&ctx, st, E_INVALIDARG) == MD_CAPTURE_ERROR &&
          ctx.active && st->duplication);

    be->stop(&ctx);
    t0 = now_ms();
    CHECK(!ctx.active && be->get_frame(&ctx, &f, 2000) == MD_CAPTURE_ERROR &&
          now_ms() - t0 < 50);
    puts("PASS stopped: get_frame -> ERROR promptly");
    CHECK(be->start(&ctx) == 0 && ctx.active);

    pointer_cells(be, &ctx);
    be->stop(&ctx);

    be->destroy(&ctx);
    CHECK(!ctx.backend_data);
    puts("ALL DXGI CAPTURE TESTS PASSED (interactive)");
    return 0;
}
