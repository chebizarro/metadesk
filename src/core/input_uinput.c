/*
 * metadesk — input_uinput.c
 * Linux input backend: uinput virtual devices.
 *
 * Creates two virtual devices via /dev/uinput:
 *   - Keyboard: EV_KEY with standard keys
 *   - Mouse: EV_KEY (buttons) + EV_ABS (position) + EV_REL (scroll)
 *
 * Requires /dev/uinput access (typically root or input group).
 */
#include "input.h"

#include <linux/uinput.h>
#include <linux/input-event-codes.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
    int      kbd_fd;
    int      mouse_fd;
    uint32_t screen_w;
    uint32_t screen_h;
} UinputState;

/* ── Helpers ─────────────────────────────────────────────────── */

static void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) { /* ignore */ }
}

static void syn(int fd) {
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void uinput_delay(void) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 }; /* 5ms */
    nanosleep(&ts, NULL);
}

/* ── Device creation ─────────────────────────────────────────── */

static int create_keyboard(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "input_uinput: cannot open /dev/uinput: %s\n", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);

    for (int k = KEY_ESC; k <= KEY_F12; k++)
        ioctl(fd, UI_SET_KEYBIT, k);
    for (int k = KEY_HOME; k <= KEY_DELETE; k++)
        ioctl(fd, UI_SET_KEYBIT, k);

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

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) { close(fd); return -1; }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }

    uinput_delay();
    return fd;
}

static int create_mouse(uint32_t screen_w, uint32_t screen_h) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "input_uinput: cannot open /dev/uinput: %s\n", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_REL);

    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);

    struct uinput_abs_setup abs_x = {
        .code = ABS_X,
        .absinfo = { .minimum = 0, .maximum = (int)screen_w - 1, .resolution = 1 },
    };
    struct uinput_abs_setup abs_y = {
        .code = ABS_Y,
        .absinfo = { .minimum = 0, .maximum = (int)screen_h - 1, .resolution = 1 },
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

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) { close(fd); return -1; }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }

    uinput_delay();
    return fd;
}

/* ── Vtable implementation ───────────────────────────────────── */

static int uinput_init(MdInputCtx *ctx, const MdInputConfig *cfg) {
    UinputState *st = calloc(1, sizeof(UinputState));
    if (!st) return -1;

    st->screen_w = (cfg && cfg->screen_width  > 0) ? cfg->screen_width  : 1920;
    st->screen_h = (cfg && cfg->screen_height > 0) ? cfg->screen_height : 1080;
    st->kbd_fd   = -1;
    st->mouse_fd = -1;

    st->kbd_fd = create_keyboard();
    if (st->kbd_fd < 0)
        fprintf(stderr, "input_uinput: WARNING — keyboard device creation failed\n");

    st->mouse_fd = create_mouse(st->screen_w, st->screen_h);
    if (st->mouse_fd < 0)
        fprintf(stderr, "input_uinput: WARNING — mouse device creation failed\n");

    ctx->backend_data = st;
    ctx->ready = (st->kbd_fd >= 0 || st->mouse_fd >= 0);

    if (!ctx->ready)
        fprintf(stderr, "input_uinput: ERROR — no virtual devices created. "
                "Check /dev/uinput permissions.\n");

    return 0; /* return success even if not ready — caller checks is_ready */
}

static int uinput_mouse_move(MdInputCtx *ctx, int x, int y) {
    UinputState *st = ctx->backend_data;
    if (!st || st->mouse_fd < 0) return -1;

    emit(st->mouse_fd, EV_ABS, ABS_X, x);
    emit(st->mouse_fd, EV_ABS, ABS_Y, y);
    syn(st->mouse_fd);
    return 0;
}

static int uinput_mouse_button(MdInputCtx *ctx, int button, int pressed) {
    UinputState *st = ctx->backend_data;
    if (!st || st->mouse_fd < 0) return -1;

    uint16_t btn;
    switch (button) {
    case 0: btn = BTN_LEFT;   break;
    case 1: btn = BTN_RIGHT;  break;
    case 2: btn = BTN_MIDDLE; break;
    default: return -1;
    }

    emit(st->mouse_fd, EV_KEY, btn, pressed ? 1 : 0);
    syn(st->mouse_fd);
    return 0;
}

static int uinput_mouse_scroll(MdInputCtx *ctx, int dx, int dy) {
    UinputState *st = ctx->backend_data;
    if (!st || st->mouse_fd < 0) return -1;

    if (dy != 0) emit(st->mouse_fd, EV_REL, REL_WHEEL, dy);
    if (dx != 0) emit(st->mouse_fd, EV_REL, REL_HWHEEL, dx);
    syn(st->mouse_fd);
    return 0;
}

static int uinput_key_event(MdInputCtx *ctx, uint32_t keysym, int pressed) {
    UinputState *st = ctx->backend_data;
    if (!st || st->kbd_fd < 0) return -1;

    /* keysym values in our system are Linux KEY_* codes directly */
    emit(st->kbd_fd, EV_KEY, (uint16_t)keysym, pressed ? 1 : 0);
    syn(st->kbd_fd);
    return 0;
}

/* Map ASCII char to keysym + shift flag. */
static uint32_t char_to_keysym(char c, bool *need_shift) {
    *need_shift = false;

    if (c >= 'a' && c <= 'z') return md_input_keysym_from_name((char[]){c, 0});
    if (c >= 'A' && c <= 'Z') {
        *need_shift = true;
        char lower = (char)(c - 'A' + 'a');
        return md_input_keysym_from_name((char[]){lower, 0});
    }
    if (c >= '0' && c <= '9') return md_input_keysym_from_name((char[]){c, 0});

    switch (c) {
    case ' ':  return md_input_keysym_from_name("space");
    case '\n': return md_input_keysym_from_name("enter");
    case '\t': return md_input_keysym_from_name("tab");
    case '-':  return md_input_keysym_from_name("-");
    case '=':  return md_input_keysym_from_name("=");
    case '[':  return md_input_keysym_from_name("[");
    case ']':  return md_input_keysym_from_name("]");
    case ';':  return md_input_keysym_from_name(";");
    case '\'': return md_input_keysym_from_name("'");
    case '`':  return md_input_keysym_from_name("`");
    case '\\': return md_input_keysym_from_name("\\");
    case ',':  return md_input_keysym_from_name(",");
    case '.':  return md_input_keysym_from_name(".");
    case '/':  return md_input_keysym_from_name("/");

    /* Shifted symbols */
    case '!': *need_shift = true; return md_input_keysym_from_name("1");
    case '@': *need_shift = true; return md_input_keysym_from_name("2");
    case '#': *need_shift = true; return md_input_keysym_from_name("3");
    case '$': *need_shift = true; return md_input_keysym_from_name("4");
    case '%': *need_shift = true; return md_input_keysym_from_name("5");
    case '^': *need_shift = true; return md_input_keysym_from_name("6");
    case '&': *need_shift = true; return md_input_keysym_from_name("7");
    case '*': *need_shift = true; return md_input_keysym_from_name("8");
    case '(': *need_shift = true; return md_input_keysym_from_name("9");
    case ')': *need_shift = true; return md_input_keysym_from_name("0");
    case '_': *need_shift = true; return md_input_keysym_from_name("-");
    case '+': *need_shift = true; return md_input_keysym_from_name("=");
    case '{': *need_shift = true; return md_input_keysym_from_name("[");
    case '}': *need_shift = true; return md_input_keysym_from_name("]");
    case ':': *need_shift = true; return md_input_keysym_from_name(";");
    case '"': *need_shift = true; return md_input_keysym_from_name("'");
    case '~': *need_shift = true; return md_input_keysym_from_name("`");
    case '|': *need_shift = true; return md_input_keysym_from_name("\\");
    case '<': *need_shift = true; return md_input_keysym_from_name(",");
    case '>': *need_shift = true; return md_input_keysym_from_name(".");
    case '?': *need_shift = true; return md_input_keysym_from_name("/");
    }

    return 0;
}

static int uinput_type_text(MdInputCtx *ctx, const char *utf8) {
    UinputState *st = ctx->backend_data;
    if (!st || st->kbd_fd < 0 || !utf8) return -1;

    uint32_t shift_sym = md_input_keysym_from_name("shift");

    for (const char *p = utf8; *p != '\0'; p++) {
        if ((unsigned char)*p > 127)
            continue; /* skip non-ASCII for now */

        bool need_shift = false;
        uint32_t sym = char_to_keysym(*p, &need_shift);
        if (sym == 0) continue;

        if (need_shift) {
            emit(st->kbd_fd, EV_KEY, (uint16_t)shift_sym, 1);
            syn(st->kbd_fd);
        }

        emit(st->kbd_fd, EV_KEY, (uint16_t)sym, 1);
        syn(st->kbd_fd);
        uinput_delay();
        emit(st->kbd_fd, EV_KEY, (uint16_t)sym, 0);
        syn(st->kbd_fd);

        if (need_shift) {
            emit(st->kbd_fd, EV_KEY, (uint16_t)shift_sym, 0);
            syn(st->kbd_fd);
        }
    }

    return 0;
}

static void uinput_destroy(MdInputCtx *ctx) {
    UinputState *st = ctx->backend_data;
    if (!st) return;

    if (st->kbd_fd >= 0) {
        ioctl(st->kbd_fd, UI_DEV_DESTROY);
        close(st->kbd_fd);
    }
    if (st->mouse_fd >= 0) {
        ioctl(st->mouse_fd, UI_DEV_DESTROY);
        close(st->mouse_fd);
    }

    free(st);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdInputBackend uinput_backend = {
    .init         = uinput_init,
    .mouse_move   = uinput_mouse_move,
    .mouse_button = uinput_mouse_button,
    .mouse_scroll = uinput_mouse_scroll,
    .key_event    = uinput_key_event,
    .type_text    = uinput_type_text,
    .destroy      = uinput_destroy,
};

const MdInputBackend *md_input_backend_create(void) {
    return &uinput_backend;
}
