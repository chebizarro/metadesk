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

/* Small delay between synthetic events for reliability. */
static void input_delay(void) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 }; /* 5ms */
    nanosleep(&ts, NULL);
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
    /* Single ASCII character fallback */
    if (name[0] != '\0' && name[1] == '\0') {
        char c = name[0];
        /* Search the table for single-char entries */
        for (const KeyNameEntry *e = key_names; e->name != NULL; e++) {
            if (e->name[0] == c && e->name[1] == '\0')
                return e->keysym;
        }
    }
    return 0;
}

/* ── Public convenience API ──────────────────────────────────── */

MdInput *md_input_create(const MdInputConfig *cfg) {
    const MdInputBackend *vtable = md_input_backend_create();
    if (!vtable) return NULL;

    MdInput *inp = calloc(1, sizeof(MdInput));
    if (!inp) return NULL;

    inp->vtable = vtable;

    if (cfg) {
        inp->config = *cfg;
    } else {
        inp->config.screen_width  = 1920;
        inp->config.screen_height = 1080;
    }

    if (inp->vtable->init(inp, &inp->config) != 0) {
        free(inp);
        return NULL;
    }

    return inp;
}

int md_input_mouse_move(MdInput *inp, int x, int y) {
    if (!inp || !inp->vtable || !inp->vtable->mouse_move) return -1;
    return inp->vtable->mouse_move(inp, x, y);
}

int md_input_click(MdInput *inp, int x, int y, int button) {
    if (!inp || !inp->vtable) return -1;

    /* Move to position */
    if (inp->vtable->mouse_move)
        inp->vtable->mouse_move(inp, x, y);

    /* Press + release */
    if (inp->vtable->mouse_button) {
        inp->vtable->mouse_button(inp, button, 1);
        input_delay();
        inp->vtable->mouse_button(inp, button, 0);
    }
    return 0;
}

int md_input_dbl_click(MdInput *inp, int x, int y) {
    int ret = md_input_click(inp, x, y, MD_MOUSE_LEFT);
    if (ret < 0) return ret;
    input_delay();
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
            fprintf(stderr, "input: unknown key name '%s'\n", keys[i]);
            return -1;
        }
    }

    /* Press keys in order */
    for (int i = 0; i < key_count; i++)
        inp->vtable->key_event(inp, syms[i], 1);

    input_delay();

    /* Release in reverse order */
    for (int i = key_count - 1; i >= 0; i--)
        inp->vtable->key_event(inp, syms[i], 0);

    return 0;
}

int md_input_type_text(MdInput *inp, const char *text) {
    if (!inp || !inp->vtable || !inp->vtable->type_text || !text) return -1;
    return inp->vtable->type_text(inp, text);
}

/* ── Action dispatch ─────────────────────────────────────────── */

int md_input_execute_action(MdInput *inp, const struct MdAction *action) {
    if (!inp || !action) return -1;

    switch (action->type) {
    case MD_ACTION_CLICK:
        return md_input_click(inp, action->region[0], action->region[1], 0);

    case MD_ACTION_DBL_CLICK:
        return md_input_dbl_click(inp, action->region[0], action->region[1]);

    case MD_ACTION_RIGHT_CLICK:
        return md_input_right_click(inp, action->region[0], action->region[1]);

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
            const char *select_all[] = { "ctrl", "a" };
            md_input_key_combo(inp, select_all, 2);
            input_delay();
            return md_input_type_text(inp, action->text);
        }
        return 0;

    case MD_ACTION_SCREENSHOT:
        return 0; /* handled at session level */

    case MD_ACTION_UNKNOWN:
    default:
        fprintf(stderr, "input: unknown action type %d\n", action->type);
        return -1;
    }
}

/* ── Lifecycle ───────────────────────────────────────────────── */

bool md_input_is_ready(const MdInput *inp) {
    return inp ? inp->ready : false;
}

void md_input_destroy(MdInput *inp) {
    if (!inp) return;
    if (inp->vtable && inp->vtable->destroy)
        inp->vtable->destroy(inp);
    free(inp);
}
