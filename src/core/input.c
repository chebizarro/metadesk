/*
 * metadesk — input.c
 * Platform-agnostic input injection convenience API.
 *
 * Implements higher-level operations (click, double-click, key combo,
 * action dispatch) on top of the backend vtable primitives.
 * The actual platform implementation lives in input_uinput.c,
 * input_cgevent.m, or input_sendinput.cpp.
 */
#include "input.h"
#include "action.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "log.h"
#define MD_LOG_TAG "input"

void md_input_settle(void) {
#ifdef _WIN32
    Sleep(MD_INPUT_EVENT_SETTLE_MS);
#else
    struct timespec ts = { .tv_sec = 0,
                           .tv_nsec = (long)MD_INPUT_EVENT_SETTLE_MS * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* ── Key name → keysym mapping (platform-neutral) ────────────── */

/* We use Linux KEY_* values as the canonical keysym space.
 * Backends translate these to their native codes as needed. */

typedef struct {
    const char *name;
    uint32_t    keysym;
} KeyNameEntry;

static const KeyNameEntry key_names[] = {
    /* Modifiers */
    { "ctrl",       0x001D }, /* KEY_LEFTCTRL  */
    { "control",    0x001D },
    { "lctrl",      0x001D },
    { "rctrl",      0x0061 }, /* KEY_RIGHTCTRL */
    { "shift",      0x002A }, /* KEY_LEFTSHIFT */
    { "lshift",     0x002A },
    { "rshift",     0x0036 }, /* KEY_RIGHTSHIFT */
    { "alt",        0x0038 }, /* KEY_LEFTALT   */
    { "lalt",       0x0038 },
    { "ralt",       0x0064 }, /* KEY_RIGHTALT  */
    { "super",      0x007D }, /* KEY_LEFTMETA  */
    { "meta",       0x007D },
    { "win",        0x007D },

    /* Special keys */
    { "enter",      0x001C }, /* KEY_ENTER     */
    { "return",     0x001C },
    { "tab",        0x000F }, /* KEY_TAB       */
    { "escape",     0x0001 }, /* KEY_ESC       */
    { "esc",        0x0001 },
    { "backspace",  0x000E }, /* KEY_BACKSPACE */
    { "delete",     0x006F }, /* KEY_DELETE    */
    { "del",        0x006F },
    { "insert",     0x006E }, /* KEY_INSERT    */
    { "space",      0x0039 }, /* KEY_SPACE     */
    { "capslock",   0x003A }, /* KEY_CAPSLOCK  */

    /* Navigation */
    { "up",         0x0067 }, /* KEY_UP        */
    { "down",       0x006C }, /* KEY_DOWN      */
    { "left",       0x0069 }, /* KEY_LEFT      */
    { "right",      0x006A }, /* KEY_RIGHT     */
    { "home",       0x0066 }, /* KEY_HOME      */
    { "end",        0x006B }, /* KEY_END       */
    { "pageup",     0x0068 }, /* KEY_PAGEUP    */
    { "pagedown",   0x006D }, /* KEY_PAGEDOWN  */

    /* F-keys (KEY_F1=0x003B .. KEY_F12=0x0058) */
    { "f1",  0x003B }, { "f2",  0x003C }, { "f3",  0x003D },
    { "f4",  0x003E }, { "f5",  0x003F }, { "f6",  0x0040 },
    { "f7",  0x0041 }, { "f8",  0x0042 }, { "f9",  0x0043 },
    { "f10", 0x0044 }, { "f11", 0x0057 }, { "f12", 0x0058 },

    /* Letters (KEY_A=0x001E .. KEY_Z) */
    { "a", 0x001E }, { "b", 0x0030 }, { "c", 0x002E }, { "d", 0x0020 },
    { "e", 0x0012 }, { "f", 0x0021 }, { "g", 0x0022 }, { "h", 0x0023 },
    { "i", 0x0017 }, { "j", 0x0024 }, { "k", 0x0025 }, { "l", 0x0026 },
    { "m", 0x0032 }, { "n", 0x0031 }, { "o", 0x0018 }, { "p", 0x0019 },
    { "q", 0x0010 }, { "r", 0x0013 }, { "s", 0x001F }, { "t", 0x0014 },
    { "u", 0x0016 }, { "v", 0x002F }, { "w", 0x0011 }, { "x", 0x002D },
    { "y", 0x0015 }, { "z", 0x002C },

    /* Digits (KEY_0=0x000B, KEY_1=0x0002 .. KEY_9=0x000A) */
    { "0", 0x000B }, { "1", 0x0002 }, { "2", 0x0003 }, { "3", 0x0004 },
    { "4", 0x0005 }, { "5", 0x0006 }, { "6", 0x0007 }, { "7", 0x0008 },
    { "8", 0x0009 }, { "9", 0x000A },

    /* Punctuation */
    { "minus",        0x000C }, /* KEY_MINUS      */
    { "-",            0x000C },
    { "equal",        0x000D }, /* KEY_EQUAL      */
    { "=",            0x000D },
    { "leftbracket",  0x001A }, /* KEY_LEFTBRACE  */
    { "[",            0x001A },
    { "rightbracket", 0x001B }, /* KEY_RIGHTBRACE */
    { "]",            0x001B },
    { "semicolon",    0x0027 }, /* KEY_SEMICOLON  */
    { ";",            0x0027 },
    { "apostrophe",   0x0028 }, /* KEY_APOSTROPHE */
    { "'",            0x0028 },
    { "grave",        0x0029 }, /* KEY_GRAVE      */
    { "`",            0x0029 },
    { "backslash",    0x002B }, /* KEY_BACKSLASH  */
    { "\\",           0x002B },
    { "comma",        0x0033 }, /* KEY_COMMA      */
    { ",",            0x0033 },
    { "period",       0x0034 }, /* KEY_DOT        */
    { ".",            0x0034 },
    { "slash",        0x0035 }, /* KEY_SLASH      */
    { "/",            0x0035 },

    { NULL, 0 }
};

uint32_t md_input_keysym_from_name(const char *name) {
    if (!name) return 0;
    for (const KeyNameEntry *e = key_names; e->name != NULL; e++) {
        if (strcasecmp(e->name, name) == 0)
            return e->keysym;
    }
    return 0;
}

/* Direct-indexed US layout for text injection; tests pin it to key_names. */
typedef struct {
    uint16_t keysym;
    bool     shift;
} AsciiKey;

#define K(sym) { (sym), false }
#define S(sym) { (sym), true }
static const AsciiKey ascii_keys[128] = {
    ['\t'] = K(0x0F), ['\n'] = K(0x1C), [' '] = K(0x39),

    ['a'] = K(0x1E), ['b'] = K(0x30), ['c'] = K(0x2E), ['d'] = K(0x20),
    ['e'] = K(0x12), ['f'] = K(0x21), ['g'] = K(0x22), ['h'] = K(0x23),
    ['i'] = K(0x17), ['j'] = K(0x24), ['k'] = K(0x25), ['l'] = K(0x26),
    ['m'] = K(0x32), ['n'] = K(0x31), ['o'] = K(0x18), ['p'] = K(0x19),
    ['q'] = K(0x10), ['r'] = K(0x13), ['s'] = K(0x1F), ['t'] = K(0x14),
    ['u'] = K(0x16), ['v'] = K(0x2F), ['w'] = K(0x11), ['x'] = K(0x2D),
    ['y'] = K(0x15), ['z'] = K(0x2C),

    ['A'] = S(0x1E), ['B'] = S(0x30), ['C'] = S(0x2E), ['D'] = S(0x20),
    ['E'] = S(0x12), ['F'] = S(0x21), ['G'] = S(0x22), ['H'] = S(0x23),
    ['I'] = S(0x17), ['J'] = S(0x24), ['K'] = S(0x25), ['L'] = S(0x26),
    ['M'] = S(0x32), ['N'] = S(0x31), ['O'] = S(0x18), ['P'] = S(0x19),
    ['Q'] = S(0x10), ['R'] = S(0x13), ['S'] = S(0x1F), ['T'] = S(0x14),
    ['U'] = S(0x16), ['V'] = S(0x2F), ['W'] = S(0x11), ['X'] = S(0x2D),
    ['Y'] = S(0x15), ['Z'] = S(0x2C),

    ['1'] = K(0x02), ['2'] = K(0x03), ['3'] = K(0x04), ['4'] = K(0x05),
    ['5'] = K(0x06), ['6'] = K(0x07), ['7'] = K(0x08), ['8'] = K(0x09),
    ['9'] = K(0x0A), ['0'] = K(0x0B),
    ['!'] = S(0x02), ['@'] = S(0x03), ['#'] = S(0x04), ['$'] = S(0x05),
    ['%'] = S(0x06), ['^'] = S(0x07), ['&'] = S(0x08), ['*'] = S(0x09),
    ['('] = S(0x0A), [')'] = S(0x0B),

    ['-']  = K(0x0C), ['_'] = S(0x0C), ['='] = K(0x0D), ['+'] = S(0x0D),
    ['[']  = K(0x1A), ['{'] = S(0x1A), [']'] = K(0x1B), ['}'] = S(0x1B),
    [';']  = K(0x27), [':'] = S(0x27), ['\''] = K(0x28), ['"'] = S(0x28),
    ['`']  = K(0x29), ['~'] = S(0x29), ['\\'] = K(0x2B), ['|'] = S(0x2B),
    [',']  = K(0x33), ['<'] = S(0x33), ['.'] = K(0x34), ['>'] = S(0x34),
    ['/']  = K(0x35), ['?'] = S(0x35),
};
#undef K
#undef S

bool md_input_ascii_key(char c, uint32_t *keysym, bool *shift) {
    unsigned char u = (unsigned char)c;
    if (u >= sizeof(ascii_keys) / sizeof(ascii_keys[0]) || !ascii_keys[u].keysym)
        return false;
    if (keysym) *keysym = ascii_keys[u].keysym;
    if (shift) *shift = ascii_keys[u].shift;
    return true;
}

/* ── Coordinate mapping ──────────────────────────────────────── */

static int clamp_coord(int v, uint32_t extent) {
    if (v < 0 || extent == 0) return 0;
    return (uint32_t)v >= extent ? (int)(extent - 1) : v;
}

void md_input_map_to_output(const MdInputConfig *cfg, int x, int y,
                            double *gx, double *gy) {
    if (!cfg || !gx || !gy) return;
    if (cfg->output_width == 0 || cfg->output_height == 0 ||
        cfg->screen_width == 0 || cfg->screen_height == 0) {
        /* Legacy: MCP mode feeds global accessibility coordinates against
         * fallback dimensions, so never clamp here. */
        *gx = x;
        *gy = y;
        return;
    }
    x = clamp_coord(x, cfg->screen_width);
    y = clamp_coord(y, cfg->screen_height);
    *gx = cfg->output_x + (double)x * cfg->output_width / cfg->screen_width;
    *gy = cfg->output_y + (double)y * cfg->output_height / cfg->screen_height;
}

/* ── Public convenience API ──────────────────────────────────── */

static bool input_dimensions_are_valid(uint32_t width, uint32_t height) {
    return width >= MD_INPUT_MIN_SCREEN_DIMENSION &&
           height >= MD_INPUT_MIN_SCREEN_DIMENSION;
}

static void input_apply_fallback_dimensions(MdInputConfig *cfg,
                                            const char *reason) {
    cfg->screen_width = MD_INPUT_FALLBACK_SCREEN_WIDTH;
    cfg->screen_height = MD_INPUT_FALLBACK_SCREEN_HEIGHT;
    MD_LOG_W("WARNING — using fallback screen dimensions %ux%u (%s)", cfg->screen_width, cfg->screen_height, reason ? reason : "actual display dimensions unavailable");
}

MdInput *md_input_create(const MdInputConfig *cfg) {
    const char *name = cfg ? cfg->backend : NULL;
    const MdInputBackend *vtable = NULL;
    bool portal_auto = false;
#ifdef MD_HAVE_PORTAL_INPUT
    if (cfg && cfg->portal_session) {
        if (name && strcmp(name, "auto") && strcmp(name, "portal")) return NULL;
        vtable = md_input_portal_backend();
    } else
#endif
    if (!name || strcmp(name, "auto") == 0) {
#if defined(__linux__) && defined(MD_HAVE_PORTAL_INPUT)
        const char *session = getenv("XDG_SESSION_TYPE");
        if (session && !strcmp(session, "wayland") && md_input_portal_available()) {
            vtable = md_input_portal_backend();
            portal_auto = true;
        } else
#endif
        vtable = md_input_backend_create();
#ifdef MD_HAVE_PORTAL_INPUT
    } else if (strcmp(name, "portal") == 0) {
        vtable = md_input_portal_backend();
#endif
#ifdef __linux__
    } else if (strcmp(name, "uinput") == 0) {
        vtable = md_input_backend_create();
#endif
#ifdef MD_HAVE_WAYLAND_INPUT
    } else if (strcmp(name, "wayland") == 0) {
        vtable = md_input_wayland_backend();
#endif
    } else {
        MD_LOG_E("input backend '%s' unavailable (no fallback)", name);
    }
    if (!vtable) return NULL;

    MdInput *inp = calloc(1, sizeof(MdInput));
    if (!inp) return NULL;

    inp->vtable = vtable;

    if (cfg) {
        inp->config = *cfg;
        if (!input_dimensions_are_valid(inp->config.screen_width,
                                        inp->config.screen_height)) {
            input_apply_fallback_dimensions(&inp->config,
                                            "configured dimensions below minimum");
        }
    } else {
        input_apply_fallback_dimensions(&inp->config, "config is NULL");
    }

    if (inp->vtable->init(inp) != 0) {
        /* Failed init owns its partial-state cleanup. Explicit overrides
         * never silently change authority/backend. Linux uinput itself
         * refuses known Wayland sessions before creating any device. */
        if (portal_auto) {
            MD_LOG_W("automatic portal input unavailable; trying platform backend");
            inp->vtable = md_input_backend_create();
        }
        if (!portal_auto || !inp->vtable || inp->vtable->init(inp) != 0) {
            free(inp);
            return NULL;
        }
    }

    return inp;
}

int md_input_set_bounds(MdInput *inp, const MdInputBounds *b) {
    if (!inp || !b ||
        !input_dimensions_are_valid(b->screen_width, b->screen_height))
        return -1;
    inp->config.screen_width  = b->screen_width;
    inp->config.screen_height = b->screen_height;
    inp->config.output_x      = b->output_x;
    inp->config.output_y      = b->output_y;
    inp->config.output_width  = b->output_width;
    inp->config.output_height = b->output_height;
    return 0;
}

int md_input_mouse_move(MdInput *inp, int x, int y) {
    if (!inp || !inp->vtable || !inp->vtable->mouse_move) return -1;
    return inp->vtable->mouse_move(inp, x, y);
}

int md_input_click(MdInput *inp, int x, int y, int button) {
    if (!inp || !inp->vtable || !inp->vtable->mouse_move ||
        !inp->vtable->mouse_button)
        return -1;

    /* Move to position */
    if (inp->vtable->mouse_move(inp, x, y) < 0)
        return -1;

    /* Press + release */
    if (inp->vtable->mouse_button(inp, button, 1) < 0)
        return -1;

    md_input_settle();

    if (inp->vtable->mouse_button(inp, button, 0) < 0)
        return -1;

    return 0;
}

int md_input_dbl_click(MdInput *inp, int x, int y) {
    int ret = md_input_click(inp, x, y, MD_MOUSE_LEFT);
    if (ret < 0) return ret;
    md_input_settle();
    return md_input_click(inp, x, y, MD_MOUSE_LEFT);
}

int md_input_right_click(MdInput *inp, int x, int y) {
    return md_input_click(inp, x, y, MD_MOUSE_RIGHT);
}

int md_input_scroll(MdInput *inp, int dx, int dy) {
    if (!inp || !inp->vtable || !inp->vtable->mouse_scroll) return -1;
    return inp->vtable->mouse_scroll(inp, dx, dy);
}

int md_input_key_combo(MdInput *inp, const char **keys, int key_count) {
    if (!inp || !inp->vtable || !inp->vtable->key_event || !keys || key_count <= 0)
        return -1;

    if (key_count > MD_INPUT_MAX_COMBO_KEYS)
        key_count = MD_INPUT_MAX_COMBO_KEYS;

    uint32_t syms[MD_INPUT_MAX_COMBO_KEYS];

    /* Resolve all key names */
    for (int i = 0; i < key_count; i++) {
        syms[i] = md_input_keysym_from_name(keys[i]);
        if (syms[i] == 0) {
            MD_LOG_I("unknown key name '%s'", keys[i]);
            return -1;
        }
    }

    /* Press keys in order. If a press fails, release any keys that were
     * already pressed before reporting failure. */
    int pressed_count = 0;
    for (int i = 0; i < key_count; i++) {
        if (inp->vtable->key_event(inp, syms[i], 1) < 0) {
            for (int j = pressed_count - 1; j >= 0; j--)
                inp->vtable->key_event(inp, syms[j], 0);
            return -1;
        }
        pressed_count++;
    }

    md_input_settle();

    /* Release in reverse order. Keep releasing remaining keys even if one
     * release fails, then report the failure to the caller. */
    int ret = 0;
    for (int i = key_count - 1; i >= 0; i--) {
        if (inp->vtable->key_event(inp, syms[i], 0) < 0)
            ret = -1;
    }

    return ret;
}

int md_input_type_text(MdInput *inp, const char *text) {
    if (!inp || !inp->vtable || !inp->vtable->type_text || !text) return -1;
    return inp->vtable->type_text(inp, text);
}

/* ── Action dispatch ───────────────────────────────────────────── */

/* Action coordinates come from the accessibility tree, i.e. global desktop
 * space, while mouse_move takes frame-relative points that backends map
 * into the output bounds. Invert that mapping; a point off the captured
 * output is refused rather than clamped onto its edge. */
static int action_point_to_frame(const MdInput *inp, int gx, int gy,
                                 int *fx, int *fy) {
    const MdInputConfig *c = &inp->config;
    if (c->output_width == 0 || c->output_height == 0 ||
        c->screen_width == 0 || c->screen_height == 0) {
        *fx = gx;
        *fy = gy;
        return 0;
    }
    const long long dx = (long long)gx - c->output_x;
    const long long dy = (long long)gy - c->output_y;
    if (dx < 0 || dy < 0 || dx >= c->output_width || dy >= c->output_height) {
        MD_LOG_E("action point (%d,%d) is outside the captured output %ux%u at (%d,%d)", gx, gy, c->output_width, c->output_height, c->output_x, c->output_y);
        return -1;
    }
    *fx = (int)(dx * c->screen_width / c->output_width);
    *fy = (int)(dy * c->screen_height / c->output_height);
    return 0;
}

int md_input_execute_action(MdInput *inp, const struct MdAction *action) {
    if (!inp || !action) return -1;

    int x = 0, y = 0;
    switch (action->type) {
    case MD_ACTION_CLICK:
    case MD_ACTION_DBL_CLICK:
    case MD_ACTION_RIGHT_CLICK:
        if (action_point_to_frame(inp, action->region[0], action->region[1],
                                  &x, &y) < 0)
            return -1;
        break;
    default:
        break;
    }

    switch (action->type) {
    case MD_ACTION_CLICK:
        return md_input_click(inp, x, y, 0);

    case MD_ACTION_DBL_CLICK:
        return md_input_dbl_click(inp, x, y);

    case MD_ACTION_RIGHT_CLICK:
        return md_input_right_click(inp, x, y);

    case MD_ACTION_TYPE:
        return md_input_type_text(inp, action->text);

    case MD_ACTION_KEY_COMBO:
        return md_input_key_combo(inp, (const char **)action->keys, action->key_count);

    case MD_ACTION_SCROLL:
        return md_input_scroll(inp, action->dx, action->dy);

    case MD_ACTION_FOCUS:
        return 0; /* resolved to click by agent layer */

    case MD_ACTION_SET_VALUE:
        if (action->text[0] != '\0') {
            /* Select-all shortcut is platform-specific: Cmd on macOS,
             * Ctrl elsewhere. This runs on the host, so pick by build OS. */
#ifdef __APPLE__
            const char *select_all[] = { "meta", "a" };
#else
            const char *select_all[] = { "ctrl", "a" };
#endif
            if (md_input_key_combo(inp, select_all, 2) < 0)
                return -1;
            md_input_settle();
            return md_input_type_text(inp, action->text);
        }
        return 0;

    case MD_ACTION_SCREENSHOT:
        return 0; /* handled at session level */

    case MD_ACTION_UNKNOWN:
    default:
        MD_LOG_I("unknown action type %d", action->type);
        return -1;
    }
}

/* ── Lifecycle ───────────────────────────────────────────────── */

bool md_input_is_ready(const MdInput *inp) {
    if (!inp) return false;
    if (inp->vtable && inp->vtable->is_ready)
        return inp->vtable->is_ready(inp);
    return inp->ready;
}

void md_input_destroy(MdInput *inp) {
    if (!inp) return;
    if (inp->vtable && inp->vtable->destroy)
        inp->vtable->destroy(inp);
    free(inp);
}
