/* metadesk — tests/test_sendinput_win.cpp (md-7yc)
 *
 * Standalone Windows unit test for the SendInput backend. SendInput is
 * replaced, in this translation unit only, by a recorder: the test checks the
 * INPUT records the backend builds and how it propagates a rejected
 * injection, and never injects real input into any session or window. That
 * makes it safe to run anywhere, including SSH (session 0) and a console
 * someone is using.
 *
 * Covers md-ty3 I6 (si_mouse_scroll ignored SendInput's result; type_text
 * did too) and I7 (lookup "not found" is the return value, not VK 0), plus
 * keymap coverage of the HAL's key vocabulary and its US ASCII table.
 *
 * md-w65: GetSystemMetrics is replaced the same way, by a synthetic desktop
 * layout (virtual desktop bounds), so mouse moves are checked
 * against multi-monitor, negative-origin and mixed-DPI layouts the test
 * machine does not have. Every recorded move is decoded the way Windows
 * decodes MOUSEEVENTF_VIRTUALDESK (pixel = left + n * width / 65536) and
 * must land on the exact intended desktop pixel.
 */
#include <windows.h>

#include <vector>

static std::vector<INPUT> sent;      /* every record the backend submitted */
static int sends = 0;                /* SendInput calls */
static int fail_on_send = 0;         /* 1-based call to reject; 0 = none */

static UINT WINAPI recorded_send_input(UINT count, LPINPUT inputs, int size) {
    if (size != (int)sizeof(INPUT)) return 0;
    if (++sends == fail_on_send) return 0;
    sent.insert(sent.end(), inputs, inputs + count);
    return count;
}

/* Synthetic physical desktop: the virtual desktop's bounding rectangle. */
static struct { int vx, vy, vw, vh; } layout;

static int WINAPI synthetic_metrics(int index) {
    switch (index) {
    case SM_XVIRTUALSCREEN:  return layout.vx;
    case SM_YVIRTUALSCREEN:  return layout.vy;
    case SM_CXVIRTUALSCREEN: return layout.vw;
    case SM_CYVIRTUALSCREEN: return layout.vh;
    default:                 return 0;
    }
}

#define SendInput recorded_send_input
#define GetSystemMetrics synthetic_metrics
#include "../src/core/input_sendinput.cpp"
#undef GetSystemMetrics
#undef SendInput

#include <cstdio>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    ExitProcess(1); \
} } while (0)

static void reset(int fail = 0) {
    sent.clear();
    sends = 0;
    fail_on_send = fail;
}

/* How Windows turns a MOUSEEVENTF_VIRTUALDESK value back into a pixel. */
static LONG decode(LONG n, int lo, int len) {
    return (LONG)(lo + (int64_t)n * len / 65536);
}

static void set_layout(int vx, int vy, int vw, int vh) {
    layout = { vx, vy, vw, vh };
}

/* Where the last recorded move lands on the synthetic desktop. */
static POINT landed() {
    const MOUSEINPUT &mi = sent.back().mi;
    CHECK(sent.back().type == INPUT_MOUSE &&
          mi.dwFlags == (MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE));
    CHECK(mi.dx >= 0 && mi.dx <= 65535 && mi.dy >= 0 && mi.dy <= 65535);
    return { decode(mi.dx, layout.vx, layout.vw), decode(mi.dy, layout.vy, layout.vh) };
}

/* A frame of w x h addressing the output at (ox,oy) ow x oh; ow == 0 is the
 * legacy configuration without output bounds. */
static MdInputCtx make_ctx(uint32_t w, uint32_t h, int32_t ox = 0, int32_t oy = 0,
                           uint32_t ow = 0, uint32_t oh = 0) {
    MdInputCtx ctx = {};
    ctx.vtable = &sendinput_backend;
    ctx.config.screen_width = w;
    ctx.config.screen_height = h;
    ctx.config.output_x = ox;
    ctx.config.output_y = oy;
    ctx.config.output_width = ow;
    ctx.config.output_height = oh;
    CHECK(sendinput_backend.init(&ctx) == 0);
    return ctx;
}

static bool moves_to(MdInputCtx *ctx, int x, int y, LONG want_x, LONG want_y) {
    reset();
    CHECK(si_mouse_move(ctx, x, y) == 0 && sent.size() == 1);
    const POINT p = landed();
    if (p.x == want_x && p.y == want_y) return true;
    fprintf(stderr, "  frame (%d,%d) landed on (%ld,%ld), want (%ld,%ld)\n", x, y, p.x, p.y,
            want_x, want_y);
    return false;
}

/* md-w65: every pixel of every virtual-desktop width round-trips through
 * the normalisation exactly, including the far edge and negative origins. */
static void test_normalize() {
    int widths = 0;
    for (int64_t len : { 64, 100, 768, 1024, 1080, 1366, 1920, 2560, 3000, 3840, 4480,
                         5120, 5760, 7680, 10240, 16384, 32767 }) {
        for (int64_t lo : { (int64_t)0, (int64_t)-1920, (int64_t)-7, (int64_t)1366 }) {
            const Span desk = { lo, len };
            for (int64_t p = lo; p < lo + len; p++) {
                const LONG n = si_normalize(p, desk);
                if (decode(n, (int)lo, (int)len) != p) {
                    fprintf(stderr, "width %lld origin %lld: pixel %lld -> %ld -> %ld\n",
                            (long long)len, (long long)lo, (long long)p, n,
                            decode(n, (int)lo, (int)len));
                    CHECK(false);
                }
            }
            /* Outside the virtual desktop clamps to its edges. */
            CHECK(decode(si_normalize(lo - 50, desk), (int)lo, (int)len) == lo);
            CHECK(decode(si_normalize(lo + len + 50, desk), (int)lo, (int)len) == lo + len - 1);
        }
        widths++;
    }
    printf("PASS normalisation: every pixel of %d virtual-desktop widths x 4 origins "
           "round-trips exactly\n", widths);
}

/* md-w65: moves land on the configured output in the synthetic layout. */
static void test_targets() {
    /* Legacy (no output bounds): frame coordinates are global desktop
     * pixels, e.g. MCP clicks at UIA coordinates. */
    set_layout(0, 0, 1920, 1080);
    MdInputCtx ctx = make_ctx(1920, 1080);
    CHECK(moves_to(&ctx, 0, 0, 0, 0) && moves_to(&ctx, 1919, 1079, 1919, 1079));
    CHECK(moves_to(&ctx, 1, 1, 1, 1) && moves_to(&ctx, 960, 540, 960, 540));
    /* Primary 1920x1080 at (0,0) and a 2560x1440 display to its left and
     * above: the virtual desktop is (-2560,-200) 4480x1440. Global points on
     * either display are reached, off-desktop ones clamp onto its edge. */
    set_layout(-2560, -200, 4480, 1440);
    CHECK(moves_to(&ctx, 0, 0, 0, 0) && moves_to(&ctx, 1919, 1079, 1919, 1079));
    CHECK(moves_to(&ctx, -2000, -150, -2000, -150) && moves_to(&ctx, 5000, 2000, 1919, 1239));
    sendinput_backend.destroy(&ctx);

    /* Capturing the left display: its corners and centre. */
    ctx = make_ctx(2560, 1440, -2560, -200, 2560, 1440);
    CHECK(moves_to(&ctx, 0, 0, -2560, -200) && moves_to(&ctx, 2559, 1439, -1, 1239));
    CHECK(moves_to(&ctx, 1280, 720, -1280, 520));
    /* Off-frame coordinates clamp onto the frame, never onto the primary. */
    CHECK(moves_to(&ctx, -40, 5000, -2560, 1239) && moves_to(&ctx, 9999, -1, -1, -200));
    /* Agent actions carry global (UIA) coordinates: a click on the captured
     * output lands on that exact pixel; one on the primary is refused. */
    MdAction click = {};
    click.type = MD_ACTION_CLICK;
    click.region[0] = -1000;
    click.region[1] = 300;
    reset();
    CHECK(md_input_execute_action(&ctx, &click) == 0 && sent.size() == 3);
    sent.resize(1);
    CHECK(landed().x == -1000 && landed().y == 300);
    click.region[0] = 100;
    reset();
    CHECK(md_input_execute_action(&ctx, &click) == -1 && sends == 0);
    sendinput_backend.destroy(&ctx);

    /* Mixed DPI: a 3840x2160 display (at 150%) right of a 1920x1080 primary
     * (100%), in physical pixels: the virtual desktop is (0,0) 5760x2160. A
     * frame of the same size lands 1:1; a frame of another size (a scaled
     * stream) is scaled onto it by md_input_map_to_output. */
    set_layout(0, 0, 5760, 2160);
    ctx = make_ctx(3840, 2160, 1920, 0, 3840, 2160);
    CHECK(moves_to(&ctx, 0, 0, 1920, 0) && moves_to(&ctx, 3839, 2159, 5759, 2159));
    CHECK(moves_to(&ctx, 1234, 567, 1920 + 1234, 567));
    sendinput_backend.destroy(&ctx);
    ctx = make_ctx(2560, 1440, 1920, 0, 3840, 2160);
    CHECK(moves_to(&ctx, 0, 0, 1920, 0) && moves_to(&ctx, 2559, 1439, 5758, 2158));
    CHECK(moves_to(&ctx, 1280, 720, 1920 + 1920, 1080));
    sendinput_backend.destroy(&ctx);

    /* A rotated (portrait) display: its desktop rectangle is already in the
     * rotated orientation and so is the captured frame (md-dbg), so the
     * mapping is the same linear one. 1080x1920 portrait above-left of the
     * primary. */
    set_layout(-1080, -840, 3000, 1920);
    ctx = make_ctx(1080, 1920, -1080, -840, 1080, 1920);
    CHECK(moves_to(&ctx, 0, 0, -1080, -840) && moves_to(&ctx, 1079, 1919, -1, 1079));
    sendinput_backend.destroy(&ctx);

    /* The layout changes under a running backend (a display was attached):
     * the virtual desktop is re-read on every move. */
    set_layout(0, 0, 1920, 1080);
    ctx = make_ctx(1920, 1080, 0, 0, 1920, 1080);
    CHECK(moves_to(&ctx, 100, 100, 100, 100));
    set_layout(-1920, 0, 3840, 1080);
    CHECK(moves_to(&ctx, 100, 100, 100, 100));

    /* No display at all: nothing is sent. */
    set_layout(0, 0, 0, 0);
    reset();
    CHECK(si_mouse_move(&ctx, 10, 10) == -1 && sends == 0);
    sendinput_backend.destroy(&ctx);
    puts("PASS targets: legacy global points, non-primary and negative-origin outputs, "
         "global-coordinate actions, mixed-DPI scaling, portrait, live layout change, "
         "no display");
}

static void test_mouse(MdInputCtx *ctx) {
    set_layout(0, 0, 1920, 1080);
    reset();
    CHECK(si_mouse_move(ctx, 0, 0) == 0 && sent.size() == 1);
    CHECK(landed().x == 0 && landed().y == 0);
    CHECK(si_mouse_move(ctx, 1919, 1079) == 0 && sent.size() == 2);
    CHECK(landed().x == 1919 && landed().y == 1079);
    reset(1);
    CHECK(si_mouse_move(ctx, 10, 10) == -1);

    const struct { int button; DWORD down, up; } buttons[] = {
        { MD_MOUSE_LEFT,   MOUSEEVENTF_LEFTDOWN,   MOUSEEVENTF_LEFTUP },
        { MD_MOUSE_RIGHT,  MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP },
        { MD_MOUSE_MIDDLE, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP },
    };
    for (const auto &b : buttons) {
        reset();
        CHECK(si_mouse_button(ctx, b.button, 1) == 0 && si_mouse_button(ctx, b.button, 0) == 0);
        CHECK(sent.size() == 2 && sent[0].mi.dwFlags == b.down && sent[1].mi.dwFlags == b.up);
        reset(1);
        CHECK(si_mouse_button(ctx, b.button, 1) == -1);
    }
    reset();
    CHECK(si_mouse_button(ctx, 3, 1) == -1 && sends == 0);
    puts("PASS mouse move/button records and rejected-injection results");
}

/* I6: both axes are attempted and either rejection fails the call. */
static void test_scroll(MdInputCtx *ctx) {
    reset();
    CHECK(si_mouse_scroll(ctx, 2, -3) == 0 && sent.size() == 2);
    CHECK(sent[0].mi.dwFlags == MOUSEEVENTF_WHEEL &&
          (int)sent[0].mi.mouseData == -3 * WHEEL_DELTA);
    CHECK(sent[1].mi.dwFlags == MOUSEEVENTF_HWHEEL &&
          (int)sent[1].mi.mouseData == 2 * WHEEL_DELTA);
    reset();
    CHECK(si_mouse_scroll(ctx, 0, 0) == 0 && sends == 0);
    reset(1);
    CHECK(si_mouse_scroll(ctx, 1, 1) == -1 && sends == 2 && sent.size() == 1 &&
          sent[0].mi.dwFlags == MOUSEEVENTF_HWHEEL);
    reset(2);
    CHECK(si_mouse_scroll(ctx, 1, 1) == -1 && sends == 2);
    reset(1);
    CHECK(si_mouse_scroll(ctx, 0, 1) == -1);
    reset(1);
    CHECK(si_mouse_scroll(ctx, 1, 0) == -1);
    puts("PASS scroll: both axes attempted, either rejection returns -1 (I6)");
}

static void test_keys(MdInputCtx *ctx) {
    /* I7: an unmapped keysym is reported by the lookup, not as VK 0. */
    WORD vk = 0xFFFF;
    CHECK(!linux_key_to_vk(0x7F, &vk) && vk == 0xFFFF);
    CHECK(!linux_key_to_vk(0, &vk));
    reset();
    CHECK(si_key_event(ctx, 0x7F, 1) == -1 && sends == 0);

    /* Every key name and every US ASCII character the HAL can produce has a
     * Windows virtual key. */
    static const char *const names[] = {
        "ctrl", "rctrl", "shift", "rshift", "alt", "ralt", "super",
        "enter", "tab", "escape", "backspace", "delete", "insert", "space",
        "capslock", "up", "down", "left", "right", "home", "end", "pageup",
        "pagedown", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9",
        "f10", "f11", "f12", "minus", "equal", "leftbracket", "rightbracket",
        "semicolon", "apostrophe", "grave", "backslash", "comma", "period",
        "slash",
    };
    for (const char *name : names) {
        uint32_t sym = md_input_keysym_from_name(name);
        if (!sym || !linux_key_to_vk(sym, &vk)) {
            fprintf(stderr, "key name %s (keysym 0x%x) has no VK\n", name, sym);
            CHECK(false);
        }
    }
    int ascii = 0;
    for (int c = 1; c < 128; c++) {
        uint32_t sym = 0;
        bool shift = false;
        if (!md_input_ascii_key((char)c, &sym, &shift)) continue;
        if (!linux_key_to_vk(sym, &vk)) {
            fprintf(stderr, "ASCII 0x%02x (keysym 0x%x) has no VK\n", c, sym);
            CHECK(false);
        }
        ascii++;
    }
    CHECK(ascii == 97); /* printable ASCII, tab and newline */

    reset();
    CHECK(si_key_event(ctx, md_input_keysym_from_name("a"), 1) == 0 &&
          si_key_event(ctx, md_input_keysym_from_name("a"), 0) == 0);
    CHECK(sent.size() == 2 && sent[0].type == INPUT_KEYBOARD && sent[0].ki.wVk == 'A' &&
          sent[0].ki.dwFlags == 0 && sent[1].ki.dwFlags == KEYEVENTF_KEYUP);
    reset();
    CHECK(si_key_event(ctx, md_input_keysym_from_name("up"), 1) == 0);
    CHECK(sent.size() == 1 && sent[0].ki.wVk == VK_UP &&
          sent[0].ki.dwFlags == KEYEVENTF_EXTENDEDKEY);
    reset(1);
    CHECK(si_key_event(ctx, md_input_keysym_from_name("a"), 1) == -1);
    printf("PASS keys: %zu names and %d ASCII characters map; unmapped keysym "
           "fails before injection (I7)\n", sizeof(names) / sizeof(names[0]), ascii);
}

static void test_text(MdInputCtx *ctx) {
    /* "a", U+00E9 and U+1F600 (a surrogate pair): one down/up per UTF-16 unit. */
    const char *text = "a\xC3\xA9\xF0\x9F\x98\x80";
    const WORD units[] = { L'a', 0x00E9, 0xD83D, 0xDE00 };
    reset();
    CHECK(si_type_text(ctx, text) == 0 && sends == 4 && sent.size() == 8);
    for (int i = 0; i < 4; i++) {
        CHECK(sent[2 * i].ki.wScan == units[i] && sent[2 * i].ki.dwFlags == KEYEVENTF_UNICODE);
        CHECK(sent[2 * i + 1].ki.wScan == units[i] &&
              sent[2 * i + 1].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
    }
    reset(2);
    CHECK(si_type_text(ctx, text) == -1 && sends == 2);
    reset();
    CHECK(si_type_text(ctx, nullptr) == -1 && si_type_text(ctx, "") == 0 && sends == 0);
    puts("PASS type_text: UTF-16 units incl. surrogates; stops and fails on rejection");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const MdInputBackend *be = md_input_backend_create();
    CHECK(be == &sendinput_backend && !be->is_ready);

    MdInputCtx ctx = {};
    ctx.vtable = be;
    ctx.config.screen_width = 1920;
    ctx.config.screen_height = 1080;
    CHECK(be->init(&ctx) == 0 && ctx.ready && ctx.backend_data);

    test_normalize();
    test_targets();
    test_mouse(&ctx);
    test_scroll(&ctx);
    test_keys(&ctx);
    test_text(&ctx);

    be->destroy(&ctx);
    CHECK(!ctx.backend_data);
    MdInputCtx narrow = {}; /* not "small": rpcndr.h #defines it */
    narrow.config.screen_width = 32;
    narrow.config.screen_height = 1080;
    CHECK(be->init(&narrow) == -1 && !narrow.backend_data);
    puts("ALL SENDINPUT TESTS PASSED (no real input injected)");
    return 0;
}
