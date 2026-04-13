/*
 * metadesk — input.c
 * uinput virtual input device injection.
 *
 * Creates two virtual devices via /dev/uinput:
 *   - Keyboard: EV_KEY with all standard keys (KEY_A..KEY_Z, modifiers, etc.)
 *   - Mouse: EV_KEY (buttons) + EV_ABS (absolute X/Y) + EV_REL (scroll)
 *
 * Key name → Linux keycode mapping covers:
 *   - Letters (a-z), digits (0-9)
 *   - Modifiers: ctrl, shift, alt, super/meta
 *   - Special: enter, tab, escape, backspace, space, delete
 *   - F-keys: f1-f12
 *   - Navigation: up, down, left, right, home, end, pageup, pagedown
 *   - Punctuation and symbols
 *
 * The uinput protocol:
 *   1. open /dev/uinput
 *   2. ioctl UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, etc.
 *   3. write struct uinput_user_dev (or ioctl UI_DEV_SETUP)
 *   4. ioctl UI_DEV_CREATE
 *   5. write struct input_event for each injection
 *   6. ioctl UI_DEV_DESTROY on cleanup
 */
#include "input.h"
#include "action.h"

#include <linux/uinput.h>
#include <linux/input-event-codes.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

struct MdInput {
    int          kbd_fd;        /* uinput fd for virtual keyboard */
    int          mouse_fd;      /* uinput fd for virtual mouse    */
    uint32_t     screen_w;
    uint32_t     screen_h;
    bool         ready;
};

/* ── Key name → Linux keycode mapping ────────────────────────── */

typedef struct {
    const char *name;
    uint16_t    code;
} KeyMap;

static const KeyMap key_map[] = {
    /* Modifiers */
    { "ctrl",       KEY_LEFTCTRL  },
    { "control",    KEY_LEFTCTRL  },
    { "lctrl",      KEY_LEFTCTRL  },
    { "rctrl",      KEY_RIGHTCTRL },
    { "shift",      KEY_LEFTSHIFT },
    { "lshift",     KEY_LEFTSHIFT },
    { "rshift",     KEY_RIGHTSHIFT},
    { "alt",        KEY_LEFTALT   },
    { "lalt",       KEY_LEFTALT   },
    { "ralt",       KEY_RIGHTALT  },
    { "super",      KEY_LEFTMETA  },
    { "meta",       KEY_LEFTMETA  },
    { "win",        KEY_LEFTMETA  },

    /* Special keys */
    { "enter",      KEY_ENTER     },
    { "return",     KEY_ENTER     },
    { "tab",        KEY_TAB       },
    { "escape",     KEY_ESC       },
    { "esc",        KEY_ESC       },
    { "backspace",  KEY_BACKSPACE },
    { "delete",     KEY_DELETE    },
    { "del",        KEY_DELETE    },
    { "insert",     KEY_INSERT    },
    { "space",      KEY_SPACE     },
    { "capslock",   KEY_CAPSLOCK  },

    /* Navigation */
    { "up",         KEY_UP        },
    { "down",       KEY_DOWN      },
    { "left",       KEY_LEFT      },
    { "right",      KEY_RIGHT     },
    { "home",       KEY_HOME      },
    { "end",        KEY_END       },
    { "pageup",     KEY_PAGEUP    },
    { "pagedown",   KEY_PAGEDOWN  },

    /* F-keys */
    { "f1",  KEY_F1  }, { "f2",  KEY_F2  }, { "f3",  KEY_F3  },
    { "f4",  KEY_F4  }, { "f5",  KEY_F5  }, { "f6",  KEY_F6  },
    { "f7",  KEY_F7  }, { "f8",  KEY_F8  }, { "f9",  KEY_F9  },
    { "f10", KEY_F10 }, { "f11", KEY_F11 }, { "f12", KEY_F12 },

    /* Letters */
    { "a", KEY_A }, { "b", KEY_B }, { "c", KEY_C }, { "d", KEY_D },
    { "e", KEY_E }, { "f", KEY_F }, { "g", KEY_G }, { "h", KEY_H },
    { "i", KEY_I }, { "j", KEY_J }, { "k", KEY_K }, { "l", KEY_L },
    { "m", KEY_M }, { "n", KEY_N }, { "o", KEY_O }, { "p", KEY_P },
    { "q", KEY_Q }, { "r", KEY_R }, { "s", KEY_S }, { "t", KEY_T },
    { "u", KEY_U }, { "v", KEY_V }, { "w", KEY_W }, { "x", KEY_X },
    { "y", KEY_Y }, { "z", KEY_Z },

    /* Digits */
    { "0", KEY_0 }, { "1", KEY_1 }, { "2", KEY_2 }, { "3", KEY_3 },
    { "4", KEY_4 }, { "5", KEY_5 }, { "6", KEY_6 }, { "7", KEY_7 },
    { "8", KEY_8 }, { "9", KEY_9 },

    /* Punctuation */
    { "minus",        KEY_MINUS      },
    { "-",            KEY_MINUS      },
    { "equal",        KEY_EQUAL      },
    { "=",            KEY_EQUAL      },
    { "leftbracket",  KEY_LEFTBRACE  },
    { "[",            KEY_LEFTBRACE  },
    { "rightbracket", KEY_RIGHTBRACE },
    { "]",            KEY_RIGHTBRACE },
    { "semicolon",    KEY_SEMICOLON  },
    { ";",            KEY_SEMICOLON  },
    { "apostrophe",   KEY_APOSTROPHE },
    { "'",            KEY_APOSTROPHE },
    { "grave",        KEY_GRAVE      },
    { "`",            KEY_GRAVE      },
    { "backslash",    KEY_BACKSLASH  },
    { "\\",           KEY_BACKSLASH  },
    { "comma",        KEY_COMMA      },
    { ",",            KEY_COMMA      },
    { "period",       KEY_DOT        },
    { ".",            KEY_DOT        },
    { "slash",        KEY_SLASH      },
    { "/",            KEY_SLASH      },

    { "printscreen",  KEY_SYSRQ      },
    { "scrolllock",   KEY_SCROLLLOCK },
    { "pause",        KEY_PAUSE      },

    { NULL, 0 }  /* sentinel */
};

/* Look up Linux keycode from key name string. Returns 0 if not found. */
static uint16_t keycode_from_name(const char *name) {
    if (!name) return 0;
    for (const KeyMap *km = key_map; km->name != NULL; km++) {
        if (strcasecmp(km->name, name) == 0)
            return km->code;
    }
    /* Single ASCII character fallback */
    if (name[0] != '\0' && name[1] == '\0') {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return KEY_A + (uint16_t)(c - 'a');
        if (c >= 'A' && c <= 'Z') return KEY_A + (uint16_t)(c - 'A');
        if (c >= '0' && c <= '9') return KEY_0 + (uint16_t)(c - '0');
    }
    return 0;
}

/* Map ASCII char to keycode + whether shift is needed. */
static uint16_t char_to_keycode(char c, bool *need_shift) {
    *need_shift = false;

    if (c >= 'a' && c <= 'z') return KEY_A + (uint16_t)(c - 'a');
    if (c >= 'A' && c <= 'Z') { *need_shift = true; return KEY_A + (uint16_t)(c - 'A'); }
    if (c >= '0' && c <= '9') return KEY_0 + (uint16_t)(c - '0');

    switch (c) {
    case ' ':  return KEY_SPACE;
    case '\n': return KEY_ENTER;
    case '\t': return KEY_TAB;
    case '-':  return KEY_MINUS;
    case '=':  return KEY_EQUAL;
    case '[':  return KEY_LEFTBRACE;
    case ']':  return KEY_RIGHTBRACE;
    case ';':  return KEY_SEMICOLON;
    case '\'': return KEY_APOSTROPHE;
    case '`':  return KEY_GRAVE;
    case '\\': return KEY_BACKSLASH;
    case ',':  return KEY_COMMA;
    case '.':  return KEY_DOT;
    case '/':  return KEY_SLASH;

    /* Shifted symbols */
    case '!': *need_shift = true; return KEY_1;
    case '@': *need_shift = true; return KEY_2;
    case '#': *need_shift = true; return KEY_3;
    case '$': *need_shift = true; return KEY_4;
    case '%': *need_shift = true; return KEY_5;
    case '^': *need_shift = true; return KEY_6;
    case '&': *need_shift = true; return KEY_7;
    case '*': *need_shift = true; return KEY_8;
    case '(': *need_shift = true; return KEY_9;
    case ')': *need_shift = true; return KEY_0;
    case '_': *need_shift = true; return KEY_MINUS;
    case '+': *need_shift = true; return KEY_EQUAL;
    case '{': *need_shift = true; return KEY_LEFTBRACE;
    case '}': *need_shift = true; return KEY_RIGHTBRACE;
    case ':': *need_shift = true; return KEY_SEMICOLON;
    case '"': *need_shift = true; return KEY_APOSTROPHE;
    case '~': *need_shift = true; return KEY_GRAVE;
    case '|': *need_shift = true; return KEY_BACKSLASH;
    case '<': *need_shift = true; return KEY_COMMA;
    case '>': *need_shift = true; return KEY_DOT;
    case '?': *need_shift = true; return KEY_SLASH;
    }

    return 0; /* unmapped */
}

/* ── uinput helpers ──────────────────────────────────────────── */

static void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    /* Timestamp is optional — kernel fills it if zero */
    if (write(fd, &ev, sizeof(ev)) < 0) {
        /* Silently ignore write errors — device may have been destroyed */
    }
}

static void syn(int fd) {
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* Small delay between synthetic events for reliability. */
static void input_delay(void) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 }; /* 5ms */
    nanosleep(&ts, NULL);
}

/* ── uinput device creation ──────────────────────────────────── */

static int create_keyboard(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "input: cannot open /dev/uinput: %s\n", strerror(errno));
        return -1;
    }

    /* Enable key events */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);

    /* Register all keys we might use (KEY_ESC through KEY_MAX is overkill,
     * but simpler than registering individually) */
    for (int k = KEY_ESC; k <= KEY_F12; k++)
        ioctl(fd, UI_SET_KEYBIT, k);
    for (int k = KEY_HOME; k <= KEY_DELETE; k++)
        ioctl(fd, UI_SET_KEYBIT, k);
    /* Modifiers */
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTCTRL);
    ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTCTRL);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
    ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTSHIFT);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTALT);
    ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTALT);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTMETA);
    ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTMETA);
    ioctl(fd, UI_SET_KEYBIT, KEY_CAPSLOCK);
    ioctl(fd, UI_SET_KEYBIT, KEY_SYSRQ);
    ioctl(fd, UI_SET_KEYBIT, KEY_SCROLLLOCK);
    ioctl(fd, UI_SET_KEYBIT, KEY_PAUSE);
    ioctl(fd, UI_SET_KEYBIT, KEY_INSERT);

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "metadesk-keyboard");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x0001;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }

    /* Allow time for device to register with the kernel */
    input_delay();
    return fd;
}

static int create_mouse(uint32_t screen_w, uint32_t screen_h) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "input: cannot open /dev/uinput: %s\n", strerror(errno));
        return -1;
    }

    /* Enable event types */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_REL);

    /* Mouse buttons */
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    /* Absolute axes for positioning */
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    /* Relative axes for scroll */
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);

    /* Configure absolute axis ranges */
    struct uinput_abs_setup abs_x = {
        .code = ABS_X,
        .absinfo = {
            .minimum = 0,
            .maximum = (int)screen_w - 1,
            .resolution = 1,
        },
    };
    struct uinput_abs_setup abs_y = {
        .code = ABS_Y,
        .absinfo = {
            .minimum = 0,
            .maximum = (int)screen_h - 1,
            .resolution = 1,
        },
    };
    ioctl(fd, UI_ABS_SETUP, &abs_x);
    ioctl(fd, UI_ABS_SETUP, &abs_y);

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "metadesk-mouse");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x0002;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }

    input_delay();
    return fd;
}

/* ── Public API ──────────────────────────────────────────────── */

MdInput *md_input_create(const MdInputConfig *cfg) {
    MdInput *inp = calloc(1, sizeof(MdInput));
    if (!inp) return NULL;

    inp->screen_w = (cfg && cfg->screen_width  > 0) ? cfg->screen_width  : 1920;
    inp->screen_h = (cfg && cfg->screen_height > 0) ? cfg->screen_height : 1080;
    inp->kbd_fd   = -1;
    inp->mouse_fd = -1;

    inp->kbd_fd = create_keyboard();
    if (inp->kbd_fd < 0) {
        fprintf(stderr, "input: WARNING — keyboard device creation failed\n");
        /* Continue — mouse may still work */
    }

    inp->mouse_fd = create_mouse(inp->screen_w, inp->screen_h);
    if (inp->mouse_fd < 0) {
        fprintf(stderr, "input: WARNING — mouse device creation failed\n");
    }

    inp->ready = (inp->kbd_fd >= 0 || inp->mouse_fd >= 0);
    if (!inp->ready) {
        fprintf(stderr, "input: ERROR — no virtual devices created. "
                "Check /dev/uinput permissions.\n");
    }

    return inp;
}

int md_input_mouse_move(MdInput *inp, int x, int y) {
    if (!inp || inp->mouse_fd < 0) return -1;

    emit(inp->mouse_fd, EV_ABS, ABS_X, x);
    emit(inp->mouse_fd, EV_ABS, ABS_Y, y);
    syn(inp->mouse_fd);
    return 0;
}

int md_input_click(MdInput *inp, int x, int y, int button) {
    if (!inp || inp->mouse_fd < 0) return -1;

    uint16_t btn;
    switch (button) {
    case 0: btn = BTN_LEFT;   break;
    case 1: btn = BTN_RIGHT;  break;
    case 2: btn = BTN_MIDDLE; break;
    default: return -1;
    }

    /* Move to position */
    emit(inp->mouse_fd, EV_ABS, ABS_X, x);
    emit(inp->mouse_fd, EV_ABS, ABS_Y, y);
    syn(inp->mouse_fd);

    /* Press */
    emit(inp->mouse_fd, EV_KEY, btn, 1);
    syn(inp->mouse_fd);
    input_delay();

    /* Release */
    emit(inp->mouse_fd, EV_KEY, btn, 0);
    syn(inp->mouse_fd);

    return 0;
}

int md_input_dbl_click(MdInput *inp, int x, int y) {
    if (!inp) return -1;
    int ret = md_input_click(inp, x, y, 0);
    if (ret < 0) return ret;
    input_delay();
    return md_input_click(inp, x, y, 0);
}

int md_input_right_click(MdInput *inp, int x, int y) {
    return md_input_click(inp, x, y, 1);
}

int md_input_scroll(MdInput *inp, int dx, int dy) {
    if (!inp || inp->mouse_fd < 0) return -1;

    if (dy != 0) {
        emit(inp->mouse_fd, EV_REL, REL_WHEEL, dy);
    }
    if (dx != 0) {
        emit(inp->mouse_fd, EV_REL, REL_HWHEEL, dx);
    }
    syn(inp->mouse_fd);
    return 0;
}

int md_input_key_combo(MdInput *inp, const char **keys, int key_count) {
    if (!inp || inp->kbd_fd < 0 || !keys || key_count <= 0)
        return -1;

    uint16_t codes[MD_MAX_KEYS];
    if (key_count > MD_MAX_KEYS)
        key_count = MD_MAX_KEYS;

    /* Resolve all key names to keycodes first */
    for (int i = 0; i < key_count; i++) {
        codes[i] = keycode_from_name(keys[i]);
        if (codes[i] == 0) {
            fprintf(stderr, "input: unknown key name '%s'\n", keys[i]);
            return -1;
        }
    }

    /* Press keys in order */
    for (int i = 0; i < key_count; i++) {
        emit(inp->kbd_fd, EV_KEY, codes[i], 1);
        syn(inp->kbd_fd);
    }

    input_delay();

    /* Release keys in reverse order */
    for (int i = key_count - 1; i >= 0; i--) {
        emit(inp->kbd_fd, EV_KEY, codes[i], 0);
        syn(inp->kbd_fd);
    }

    return 0;
}

int md_input_type_text(MdInput *inp, const char *text) {
    if (!inp || inp->kbd_fd < 0 || !text)
        return -1;

    for (const char *p = text; *p != '\0'; p++) {
        /* Skip non-ASCII for now — UTF-8 multi-byte requires XKB or ibus */
        if ((unsigned char)*p > 127)
            continue;

        bool need_shift = false;
        uint16_t code = char_to_keycode(*p, &need_shift);
        if (code == 0)
            continue;

        if (need_shift) {
            emit(inp->kbd_fd, EV_KEY, KEY_LEFTSHIFT, 1);
            syn(inp->kbd_fd);
        }

        emit(inp->kbd_fd, EV_KEY, code, 1);
        syn(inp->kbd_fd);
        input_delay();
        emit(inp->kbd_fd, EV_KEY, code, 0);
        syn(inp->kbd_fd);

        if (need_shift) {
            emit(inp->kbd_fd, EV_KEY, KEY_LEFTSHIFT, 0);
            syn(inp->kbd_fd);
        }
    }

    return 0;
}

/* ── Action dispatch ─────────────────────────────────────────── */

int md_input_execute_action(MdInput *inp, const struct MdAction *action) {
    if (!inp || !action)
        return -1;

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
        /* Focus is typically done by clicking the target element.
         * The target_id would be resolved to coordinates via AT-SPI2. */
        /* TODO: resolve target_id to coordinates via AT-SPI2 (M1.7) */
        return 0;

    case MD_ACTION_SET_VALUE:
        /* Set value: focus target, select all, type new value. */
        /* TODO: AT-SPI2 set_value for accessible elements (M1.7) */
        if (action->text[0] != '\0') {
            const char *select_all[] = { "ctrl", "a" };
            md_input_key_combo(inp, select_all, 2);
            input_delay();
            return md_input_type_text(inp, action->text);
        }
        return 0;

    case MD_ACTION_SCREENSHOT:
        /* Screenshot is handled at the session level, not input */
        return 0;

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

    if (inp->kbd_fd >= 0) {
        ioctl(inp->kbd_fd, UI_DEV_DESTROY);
        close(inp->kbd_fd);
    }
    if (inp->mouse_fd >= 0) {
        ioctl(inp->mouse_fd, UI_DEV_DESTROY);
        close(inp->mouse_fd);
    }

    free(inp);
}
