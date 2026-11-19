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

#define SendInput recorded_send_input
#include "../src/core/input_sendinput.cpp"
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

static void test_mouse(MdInputCtx *ctx) {
    reset();
    CHECK(si_mouse_move(ctx, 0, 0) == 0 && sent.size() == 1);
    CHECK(sent[0].type == INPUT_MOUSE && sent[0].mi.dx == 0 && sent[0].mi.dy == 0 &&
          sent[0].mi.dwFlags == (MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE));
    CHECK(si_mouse_move(ctx, 1919, 1079) == 0 && sent.size() == 2);
    CHECK(sent[1].mi.dx == 65535 && sent[1].mi.dy == 65535);
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
