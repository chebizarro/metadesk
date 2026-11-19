/*
 * metadesk — input_sendinput.cpp
 * Windows input backend: SendInput API (spec §2.3.3).
 *
 * Uses:
 *   - SendInput with INPUT_MOUSE for mouse move/button/scroll
 *   - SendInput with INPUT_KEYBOARD for key events
 *   - SendInput with KEYEVENTF_UNICODE for type_text (Unicode)
 *
 * Mouse movement uses MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
 * MOUSEEVENTF_MOVE. md_input_map_to_output (input.c) maps the client's
 * frame-relative point into config.output_* (the captured output:
 * md_capture_get_output_rect); without output bounds the point is already
 * global. That desktop pixel is then normalised to 0-65535 across
 * the virtual desktop (SM_*VIRTUALSCREEN, which may start at negative
 * coordinates). Both are physical pixels: DXGI publishes the output in
 * physical pixels and the virtual desktop is read per-monitor DPI aware
 * (win_dpi.h), so mixed-DPI layouts map exactly. Windows turns a normalised
 * value n back into the pixel n * width / 65536, rounded down (Microsoft's
 * UI Automation client, Input.cs, relies on the same rule), so the backend
 * aims at the centre of the pixel: the result is that pixel whichever way
 * the system rounds, never its neighbour.
 *
 * Key events translate Linux KEY_* scan codes to Windows virtual
 * key codes (VK_*) via a mapping table.
 *
 * Requires Windows XP+ (SendInput is available since Win2000).
 */
extern "C" {
#include "input.h"
}

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include "log.h"
#include "win_dpi.h"
#define MD_LOG_TAG "input_sendinput"

/* ── Backend-private state ───────────────────────────────────────────── */

struct SendInputState {
    uint32_t screen_w;
    uint32_t screen_h;
};

/* ── Virtual-desktop normalisation (pure) ───────────────────────────── */

/* One axis of the virtual desktop: first pixel and pixel count. */
struct Span {
    int64_t lo, len;
};

/* Desktop pixel p (clamped into the virtual desktop) to the 0-65535 value
 * Windows maps back onto p: the centre of p's n * len / 65536 block. */
static LONG si_normalize(int64_t p, Span desk) {
    const int64_t q = p < desk.lo ? 0 : p >= desk.lo + desk.len ? desk.len - 1 : p - desk.lo;
    const int64_t n = (2 * q + 1) * 65536 / (2 * desk.len);
    return (LONG)(n > 65535 ? 65535 : n);
}

/* The virtual desktop SendInput normalises against, in physical pixels.
 * False if it is empty (no attached display). */
static bool si_virtual_desktop(Span *dx, Span *dy) {
    MdPhysicalPixels physical;
    *dx = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN) };
    *dy = { GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    return dx->len > 0 && dy->len > 0;
}

/* ── Linux KEY_* → Windows VK_* translation ──────────────────── */

struct KeyMapEntry {
    uint32_t linux_key;
    WORD     vk;
};

static const KeyMapEntry key_map[] = {
    /* Modifiers */
    { 0x001D, VK_LCONTROL },  /* KEY_LEFTCTRL  */
    { 0x0061, VK_RCONTROL },  /* KEY_RIGHTCTRL */
    { 0x002A, VK_LSHIFT   },  /* KEY_LEFTSHIFT */
    { 0x0036, VK_RSHIFT   },  /* KEY_RIGHTSHIFT */
    { 0x0038, VK_LMENU    },  /* KEY_LEFTALT   */
    { 0x0064, VK_RMENU    },  /* KEY_RIGHTALT  */
    { 0x007D, VK_LWIN     },  /* KEY_LEFTMETA  */

    /* Special keys */
    { 0x001C, VK_RETURN    },  /* KEY_ENTER     */
    { 0x000F, VK_TAB       },  /* KEY_TAB       */
    { 0x0001, VK_ESCAPE    },  /* KEY_ESC       */
    { 0x000E, VK_BACK      },  /* KEY_BACKSPACE */
    { 0x006F, VK_DELETE    },  /* KEY_DELETE    */
    { 0x006E, VK_INSERT    },  /* KEY_INSERT    */
    { 0x0039, VK_SPACE     },  /* KEY_SPACE     */
    { 0x003A, VK_CAPITAL   },  /* KEY_CAPSLOCK  */

    /* Navigation */
    { 0x0067, VK_UP        },  /* KEY_UP        */
    { 0x006C, VK_DOWN      },  /* KEY_DOWN      */
    { 0x0069, VK_LEFT      },  /* KEY_LEFT      */
    { 0x006A, VK_RIGHT     },  /* KEY_RIGHT     */
    { 0x0066, VK_HOME      },  /* KEY_HOME      */
    { 0x006B, VK_END       },  /* KEY_END       */
    { 0x0068, VK_PRIOR     },  /* KEY_PAGEUP    */
    { 0x006D, VK_NEXT      },  /* KEY_PAGEDOWN  */

    /* F-keys */
    { 0x003B, VK_F1  }, { 0x003C, VK_F2  }, { 0x003D, VK_F3  },
    { 0x003E, VK_F4  }, { 0x003F, VK_F5  }, { 0x0040, VK_F6  },
    { 0x0041, VK_F7  }, { 0x0042, VK_F8  }, { 0x0043, VK_F9  },
    { 0x0044, VK_F10 }, { 0x0057, VK_F11 }, { 0x0058, VK_F12 },

    /* Letters (VK_A=0x41 .. VK_Z=0x5A, same as ASCII uppercase) */
    { 0x001E, 0x41 }, { 0x0030, 0x42 }, { 0x002E, 0x43 }, { 0x0020, 0x44 },
    { 0x0012, 0x45 }, { 0x0021, 0x46 }, { 0x0022, 0x47 }, { 0x0023, 0x48 },
    { 0x0017, 0x49 }, { 0x0024, 0x4A }, { 0x0025, 0x4B }, { 0x0026, 0x4C },
    { 0x0032, 0x4D }, { 0x0031, 0x4E }, { 0x0018, 0x4F }, { 0x0019, 0x50 },
    { 0x0010, 0x51 }, { 0x0013, 0x52 }, { 0x001F, 0x53 }, { 0x0014, 0x54 },
    { 0x0016, 0x55 }, { 0x002F, 0x56 }, { 0x0011, 0x57 }, { 0x002D, 0x58 },
    { 0x0015, 0x59 }, { 0x002C, 0x5A },

    /* Digits (VK_0=0x30 .. VK_9=0x39) */
    { 0x000B, 0x30 }, { 0x0002, 0x31 }, { 0x0003, 0x32 }, { 0x0004, 0x33 },
    { 0x0005, 0x34 }, { 0x0006, 0x35 }, { 0x0007, 0x36 }, { 0x0008, 0x37 },
    { 0x0009, 0x38 }, { 0x000A, 0x39 },

    /* Punctuation */
    { 0x000C, VK_OEM_MINUS  },  /* KEY_MINUS      */
    { 0x000D, VK_OEM_PLUS   },  /* KEY_EQUAL (=+) */
    { 0x001A, VK_OEM_4      },  /* KEY_LEFTBRACE  [{  */
    { 0x001B, VK_OEM_6      },  /* KEY_RIGHTBRACE ]}  */
    { 0x0027, VK_OEM_1      },  /* KEY_SEMICOLON  ;:  */
    { 0x0028, VK_OEM_7      },  /* KEY_APOSTROPHE '"  */
    { 0x0029, VK_OEM_3      },  /* KEY_GRAVE      `~  */
    { 0x002B, VK_OEM_5      },  /* KEY_BACKSLASH  \|  */
    { 0x0033, VK_OEM_COMMA  },  /* KEY_COMMA      ,<  */
    { 0x0034, VK_OEM_PERIOD },  /* KEY_DOT        .>  */
    { 0x0035, VK_OEM_2      },  /* KEY_SLASH      /?  */

    { 0, 0 } /* sentinel */
};

/* "Not found" is the return value, never an in-band keycode (same
 * convention as input_cgevent.m, where keycode 0 is a real key). */
static bool linux_key_to_vk(uint32_t keysym, WORD *vk) {
    for (const KeyMapEntry *m = key_map; m->linux_key != 0; m++) {
        if (m->linux_key == keysym) {
            *vk = m->vk;
            return true;
        }
    }
    return false;
}

/* Check if keysym is an extended key (needs KEYEVENTF_EXTENDEDKEY) */
static bool is_extended_key(WORD vk) {
    switch (vk) {
    case VK_RCONTROL: case VK_RMENU:
    case VK_INSERT: case VK_DELETE:
    case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT:
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

/* ── Vtable implementation ───────────────────────────────────── */

static bool si_dimensions_are_valid(const MdInputConfig *cfg) {
    return cfg->screen_width >= MD_INPUT_MIN_SCREEN_DIMENSION &&
           cfg->screen_height >= MD_INPUT_MIN_SCREEN_DIMENSION;
}

static int si_init(MdInputCtx *ctx) {
    const MdInputConfig *cfg = &ctx->config;
    if (!si_dimensions_are_valid(cfg)) {
        MD_LOG_E("ERROR — screen dimensions must be configured and >= %u (got %ux%u)", MD_INPUT_MIN_SCREEN_DIMENSION, cfg->screen_width, cfg->screen_height);
        return -1;
    }

    auto *st = (SendInputState *)calloc(1, sizeof(SendInputState));
    if (!st) return -1;

    st->screen_w = cfg->screen_width;
    st->screen_h = cfg->screen_height;
    if (cfg->output_width && cfg->output_height)
        MD_LOG_I("frame %ux%u maps onto the desktop at (%d,%d) %ux%u", st->screen_w,
                 st->screen_h, cfg->output_x, cfg->output_y, cfg->output_width,
                 cfg->output_height);

    ctx->backend_data = st;
    ctx->ready = true;
    return 0;
}

static int si_mouse_move(MdInputCtx *ctx, int x, int y) {
    auto *st = (SendInputState *)ctx->backend_data;
    if (!st || st->screen_w < MD_INPUT_MIN_SCREEN_DIMENSION ||
        st->screen_h < MD_INPUT_MIN_SCREEN_DIMENSION)
        return -1;

    Span dx, dy;
    if (!si_virtual_desktop(&dx, &dy)) return -1;
    double gx = 0, gy = 0; /* the desktop pixel, from its top-left corner */
    md_input_map_to_output(&ctx->config, x, y, &gx, &gy);

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = si_normalize((int64_t)std::floor(gx), dx);
    input.mi.dy = si_normalize((int64_t)std::floor(gy), dy);
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;

    return (SendInput(1, &input, sizeof(INPUT)) == 1) ? 0 : -1;
}

static int si_mouse_button(MdInputCtx *ctx, int button, int pressed) {
    (void)ctx;

    INPUT input = {};
    input.type = INPUT_MOUSE;

    switch (button) {
    case 0: /* left */
        input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 1: /* right */
        input.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case 2: /* middle */
        input.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    default:
        return -1;
    }

    return (SendInput(1, &input, sizeof(INPUT)) == 1) ? 0 : -1;
}

/* One wheel event: positive is up (WHEEL) or right (HWHEEL), as in the HAL. */
static bool si_wheel(DWORD flag, int detents) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    input.mi.mouseData = (DWORD)(detents * WHEEL_DELTA);
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

static int si_mouse_scroll(MdInputCtx *ctx, int dx, int dy) {
    (void)ctx;
    /* Both axes are attempted; either one failing fails the call. */
    bool ok = true;
    if (dy != 0) ok = si_wheel(MOUSEEVENTF_WHEEL, dy) && ok;
    if (dx != 0) ok = si_wheel(MOUSEEVENTF_HWHEEL, dx) && ok;
    return ok ? 0 : -1;
}

static int si_key_event(MdInputCtx *ctx, uint32_t keysym, int pressed) {
    (void)ctx;

    WORD vk = 0;
    if (!linux_key_to_vk(keysym, &vk)) {
        MD_LOG_I("unknown keysym 0x%04x", keysym);
        return -1;
    }

    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = pressed ? 0 : KEYEVENTF_KEYUP;

    if (is_extended_key(vk))
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;

    return (SendInput(1, &input, sizeof(INPUT)) == 1) ? 0 : -1;
}

static int si_type_text(MdInputCtx *ctx, const char *utf8) {
    (void)ctx;
    if (!utf8) return -1;

    /* Convert UTF-8 to wide string */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wlen <= 0) return -1;

    auto *wstr = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wstr) return -1;

    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr, wlen);

    /* Send each UTF-16 unit via KEYEVENTF_UNICODE (layout independent).
     * Stop at the first rejected injection, like every other primitive. */
    int ret = 0;
    for (int i = 0; i < wlen - 1 && ret == 0; i++) { /* -1 skips the NUL */
        INPUT inputs[2] = {};

        /* Key down */
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = (WORD)wstr[i];
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;

        /* Key up */
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = (WORD)wstr[i];
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

        if (SendInput(2, inputs, sizeof(INPUT)) != 2) ret = -1;
        else md_input_settle();
    }

    free(wstr);
    return ret;
}

static void si_destroy(MdInputCtx *ctx) {
    auto *st = (SendInputState *)ctx->backend_data;
    if (!st) return;
    free(st);
    ctx->backend_data = nullptr;
}

/* ── Singleton vtable ────────────────────────────────────────── */

/* Positional: must match the MdInputBackend member order in input.h. */
static const MdInputBackend sendinput_backend = {
    si_init,
    si_mouse_move,
    si_mouse_button,
    si_mouse_scroll,
    si_key_event,
    si_type_text,
    si_destroy,
    nullptr, /* is_ready: stateless, ctx->ready is set by init */
};

extern "C"
const MdInputBackend *md_input_backend_create(void) {
    return &sendinput_backend;
}

#else /* !_WIN32 */

extern "C"
const MdInputBackend *md_input_backend_create(void) {
    MD_LOG_I("SendInput backend not available on this platform");
    return nullptr;
}

#endif /* _WIN32 */
