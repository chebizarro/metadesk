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

#include <libudev.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/xinput.h>
#include <X11/Xauth.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include "log.h"
#include "md_time.h"
#define MD_LOG_TAG "input_uinput"

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
    int      kbd_fd;
    int      mouse_fd;
    uint32_t screen_w;   /* ABS_X/ABS_Y range, fixed at device creation */
    uint32_t screen_h;
} UinputState;

/* The device's absolute range is fixed when it is created. After
 * md_input_set_bounds() changes the frame size, scale frame points into that
 * range: the compositor maps the range proportionally either way. */
static int32_t uinput_abs(int v, uint32_t frame, uint32_t range) {
    if (frame == 0 || frame == range) return v;
    return (int32_t)((int64_t)v * range / frame);
}

/* ── Helpers ─────────────────────────────────────────────────── */

static int emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
        return -1;
    return 0;
}

static int syn(int fd) {
    return emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* ── Device creation ───────────────────────────────────────────────────────── */

/* Bound on waiting for udev to announce a new device; normally milliseconds. */
#define UINPUT_UDEV_SETTLE_MS 1000

/* True for the udev "add" of the evdev node whose parent is input device
 * @sysname (e.g. event7 under input42) — what libinput consumers open. */
static bool uinput_is_our_event_node(struct udev_device *dev, const char *sysname) {
    const char *action = udev_device_get_action(dev);
    const char *name = udev_device_get_sysname(dev);
    struct udev_device *parent = udev_device_get_parent(dev); /* owned by dev */
    const char *parent_name = parent ? udev_device_get_sysname(parent) : NULL;
    return action && strcmp(action, "add") == 0 && name &&
           strncmp(name, "event", 5) == 0 && udev_device_get_devnode(dev) &&
           parent_name && strcmp(parent_name, sysname) == 0;
}

/* Xorg probes and closes evdev before its final DEVICE_ON open. IN_OPEN
 * (even after udev add) therefore is not an acknowledgement. XI2 reports
 * enabled only after DEVICE_ON succeeded. Match Device Node, not a reusable
 * device name or XI id, and subscribe before creation to avoid lost wakeups.
 * There is no equivalent generic uinput acknowledgement on Wayland: use the
 * RemoteDesktop portal there. Headless/no-udevd callers retain kernel-only
 * creation semantics, without claiming desktop delivery. */
typedef struct {
    xcb_connection_t *connection;
    uint8_t opcode;
    xcb_atom_t node_atom;
    int64_t deadline;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t watchdog;
    int watchdog_fd;
    bool watchdog_started, done;
} UinputConsumer;

/* libxcb also waits inside connection setup/get_setup. Shutdown of a dup of
 * the same socket interrupts those waits without unsafe fork-after-threads
 * or cancellation inside libxcb. The watchdog only enforces failure; normal
 * readiness signals its condvar and joins immediately. */
static void *consumer_watchdog(void *data) {
    UinputConsumer *c = data;
    pthread_mutex_lock(&c->mutex);
    while (!c->done && md_now_ms() < c->deadline)
        md_cond_timedwait_until(&c->changed, &c->mutex, (uint64_t)c->deadline * 1000000);
    if (!c->done) shutdown(c->watchdog_fd, SHUT_RDWR);
    pthread_mutex_unlock(&c->mutex);
    return NULL;
}

static void consumer_close(UinputConsumer *c) {
    if (c->watchdog_started) {
        pthread_mutex_lock(&c->mutex); c->done = true;
        pthread_cond_signal(&c->changed); pthread_mutex_unlock(&c->mutex);
        pthread_join(c->watchdog, NULL);
        close(c->watchdog_fd);
        pthread_cond_destroy(&c->changed); pthread_mutex_destroy(&c->mutex);
    }
    if (c->connection) xcb_disconnect(c->connection);
}

static void *consumer_reply(UinputConsumer *c, unsigned sequence) {
    void *reply = NULL;
    xcb_generic_error_t *error = NULL;
    xcb_flush(c->connection);
    for (;;) {
        if (xcb_poll_for_reply(c->connection, sequence, &reply, &error)) {
            if (error) { free(reply); reply = NULL; }
            free(error);
            return reply;
        }
        int64_t left = c->deadline - md_now_ms();
        if (left <= 0 || xcb_connection_has_error(c->connection)) break;
        struct pollfd pfd = { .fd = xcb_get_file_descriptor(c->connection), .events = POLLIN };
        int n = poll(&pfd, 1, (int)left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
    }
    xcb_discard_reply(c->connection, sequence);
    return NULL;
}

static bool consumer_watch(UinputConsumer *c) {
    int screen, display;
    char *host = NULL;
    c->deadline = md_now_ms() + 2000;
    if (!xcb_parse_display(NULL, &host, &display, &screen)) return false;
    /* Only the local X server can acknowledge this kernel's evdev device. */
    bool local = !host[0] || !strcmp(host, "unix");
    free(host);
    if (!local) return false;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int n = snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/.X11-unix/X%d", display);
    if (n < 0 || n >= (int)sizeof(address.sun_path)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) { close(fd); return false; }
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int rc = -1;
        for (;;) {
            int64_t left = c->deadline - md_now_ms();
            if (left <= 0) break;
            rc = poll(&pfd, 1, (int)left);
            if (rc < 0 && errno == EINTR) continue;
            break;
        }
        int error = 0; socklen_t size = sizeof(error);
        if (rc <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) { close(fd); return false; }
    }
    c->watchdog_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (c->watchdog_fd < 0) { close(fd); return false; }
    if (pthread_mutex_init(&c->mutex, NULL)) { close(c->watchdog_fd); close(fd); return false; }
    if (md_cond_init_monotonic(&c->changed)) {
        pthread_mutex_destroy(&c->mutex); close(c->watchdog_fd); close(fd); return false;
    }
    if (pthread_create(&c->watchdog, NULL, consumer_watchdog, c)) {
        pthread_cond_destroy(&c->changed); pthread_mutex_destroy(&c->mutex);
        close(c->watchdog_fd); close(fd); return false;
    }
    c->watchdog_started = true;
    char hostname[256], display_name[32];
    snprintf(display_name, sizeof(display_name), "%d", display);
    Xauth *auth = NULL;
    char *types[] = { "MIT-MAGIC-COOKIE-1" }; int lengths[] = { 18 };
    if (!gethostname(hostname, sizeof(hostname))) {
        hostname[sizeof(hostname)-1] = 0;
        auth = XauGetBestAuthByAddr(FamilyLocal, (unsigned short)strlen(hostname), hostname,
            (unsigned short)strlen(display_name), display_name, 1, types, lengths);
    }
    xcb_auth_info_t info = {0};
    if (auth) info = (xcb_auth_info_t){ .namelen = auth->name_length, .name = auth->name,
                                       .datalen = auth->data_length, .data = auth->data };
    c->connection = xcb_connect_to_fd(fd, auth ? &info : NULL); /* takes fd */
    if (auth) XauDisposeAuth(auth);
    if (!c->connection || xcb_connection_has_error(c->connection)) return false;
    xcb_query_extension_reply_t *ext = consumer_reply(c,
        xcb_query_extension(c->connection, 15, "XInputExtension").sequence);
    if (!ext) return false;
    bool present = ext->present;
    c->opcode = ext->major_opcode; free(ext);
    if (!present) return false;
    xcb_input_xi_query_version_reply_t *version = consumer_reply(c,
        xcb_input_xi_query_version(c->connection, 2, 0).sequence);
    if (!version) return false;
    bool supported = version->major_version >= 2;
    free(version);
    if (!supported) return false;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c->connection));
    while (screen-- > 0 && it.rem) xcb_screen_next(&it);
    if (!it.rem) return false;
    struct { xcb_input_event_mask_t header; uint32_t bits; } mask = {
        { XCB_INPUT_DEVICE_ALL, 1 }, XCB_INPUT_XI_EVENT_MASK_HIERARCHY
    };
    xcb_input_xi_select_events(c->connection, it.data->root, 1, &mask.header);
    xcb_intern_atom_reply_t *atom = consumer_reply(c,
        xcb_intern_atom(c->connection, 0, 11, "Device Node").sequence);
    if (!atom) return false;
    c->node_atom = atom->atom;
    free(atom);
    return true;
}

static bool consumer_enabled(UinputConsumer *c, const char *node, const char *name) {
    xcb_input_xi_query_device_reply_t *reply = consumer_reply(c,
        xcb_input_xi_query_device(c->connection, XCB_INPUT_DEVICE_ALL).sequence);
    if (!reply) return false;
    bool found = false;
    xcb_input_xi_device_info_iterator_t it = xcb_input_xi_query_device_infos_iterator(reply);
    for (; it.rem && !found && md_now_ms() < c->deadline; xcb_input_xi_device_info_next(&it)) {
        /* eventN can be reused while Xorg still exposes the old XI device.
         * The per-create name fences that stale enabled object as well. */
        if (it.data->name_len != strlen(name) ||
            memcmp(xcb_input_xi_device_info_name(it.data), name, strlen(name))) continue;
        if (!it.data->enabled || (it.data->type != XCB_INPUT_DEVICE_TYPE_SLAVE_KEYBOARD &&
                                  it.data->type != XCB_INPUT_DEVICE_TYPE_SLAVE_POINTER)) continue;
        xcb_input_xi_get_property_reply_t *prop = consumer_reply(c,
            xcb_input_xi_get_property(c->connection, it.data->deviceid, 0,
                c->node_atom, XCB_ATOM_STRING, 0, 256).sequence);
        if (!prop) continue;
        found = prop->type == XCB_ATOM_STRING && prop->format == 8 &&
            prop->num_items == strlen(node) && !prop->bytes_after &&
            memcmp(xcb_input_xi_get_property_items(prop), node, strlen(node)) == 0;
        free(prop);
    }
    free(reply);
    return found;
}

static bool consumer_wait(UinputConsumer *c, const char *node, const char *name) {
    if (consumer_enabled(c, node, name)) return true;
    while (md_now_ms() < c->deadline) {
        xcb_generic_event_t *event;
        bool changed = false;
        while (md_now_ms() < c->deadline && (event = xcb_poll_for_event(c->connection))) {
            xcb_ge_generic_event_t *ge = (void *)event;
            if ((event->response_type & 0x7f) == XCB_GE_GENERIC &&
                ge->extension == c->opcode && ge->event_type == XCB_INPUT_HIERARCHY)
                changed = true;
            free(event);
        }
        if (changed) {
            if (consumer_enabled(c, node, name)) return true;
            /* Reply reads can also queue hierarchy events. Drain those before
             * waiting on the socket, which may no longer be readable. */
            continue;
        }
        int64_t left = c->deadline - md_now_ms();
        if (left <= 0 || xcb_connection_has_error(c->connection)) break;
        struct pollfd pfd = { .fd = xcb_get_file_descriptor(c->connection), .events = POLLIN };
        int n = poll(&pfd, 1, (int)left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
    }
    return false;
}

static int uinput_dev_create(int fd, const char *label, const char *name) {
    struct udev *udev = udev_new();
    struct udev_monitor *mon = NULL;
    if (udev) {
        struct udev_queue *queue = udev_queue_new(udev);
        bool udevd = queue && udev_queue_get_udev_is_active(queue);
        if (queue) udev_queue_unref(queue);
        if (udevd) mon = udev_monitor_new_from_netlink(udev, "udev");
        if (mon && (udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL) < 0 ||
                    udev_monitor_enable_receiving(mon) < 0)) {
            udev_monitor_unref(mon);
            mon = NULL;
        }
    }

    UinputConsumer consumer = {0};
    bool desktop = getenv("DISPLAY") && getenv("DISPLAY")[0];
    const char *session_type = getenv("XDG_SESSION_TYPE");
    if ((getenv("WAYLAND_DISPLAY") && getenv("WAYLAND_DISPLAY")[0]) ||
        (session_type && !strcmp(session_type, "wayland")) ||
        (desktop && !consumer_watch(&consumer))) {
        MD_LOG_E("%s: no acknowledged X11 consumer; use portal input on Wayland", label);
        consumer_close(&consumer);
        if (mon) udev_monitor_unref(mon);
        if (udev) udev_unref(udev);
        return -1;
    }
    char node[256] = {0};
    int rc = ioctl(fd, UI_DEV_CREATE);
    char sysname[64] = {0};
    if (rc == 0 && mon &&
        ioctl(fd, UI_GET_SYSNAME(sizeof(sysname) - 1), sysname) >= 0) {
        struct pollfd pfd = { .fd = udev_monitor_get_fd(mon), .events = POLLIN };
        int64_t deadline = md_now_ms() + UINPUT_UDEV_SETTLE_MS;
        bool settled = false;
        while (!settled) {
            int64_t left = deadline - md_now_ms();
            if (left <= 0) break;
            int n = poll(&pfd, 1, (int)left);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            struct udev_device *dev;
            /* Unrelated input hotplug traffic must not outlast the deadline. */
            while (!settled && md_now_ms() < deadline &&
                   (dev = udev_monitor_receive_device(mon)) != NULL) {
                settled = uinput_is_our_event_node(dev, sysname);
                if (settled) snprintf(node, sizeof(node), "%s", udev_device_get_devnode(dev));
                udev_device_unref(dev);
            }
        }
        if (!settled)
            MD_LOG_W("%s: udev did not announce %s within %d ms", label, sysname,
                     UINPUT_UDEV_SETTLE_MS);
    }
    if (rc == 0 && desktop && (!node[0] || !consumer_wait(&consumer, node, name))) {
        MD_LOG_E("%s: desktop did not enable the new evdev node", label);
        ioctl(fd, UI_DEV_DESTROY);
        rc = -1;
    }
    consumer_close(&consumer);
    if (mon) udev_monitor_unref(mon);
    if (udev) udev_unref(udev);
    return rc;
}

static int create_keyboard(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        MD_LOG_E("cannot open /dev/uinput: %s", strerror(errno));
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
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "metadesk-keyboard-%ld-%llu",
             (long)getpid(), (unsigned long long)md_monotonic_ns());
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x0001;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) { close(fd); return -1; }
    if (uinput_dev_create(fd, "keyboard", setup.name) < 0) { close(fd); return -1; }
    return fd;
}

static int create_mouse(uint32_t screen_w, uint32_t screen_h) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        MD_LOG_I("cannot open /dev/uinput: %s", strerror(errno));
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
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "metadesk-mouse-%ld-%llu",
             (long)getpid(), (unsigned long long)md_monotonic_ns());
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x0002;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) { close(fd); return -1; }
    if (uinput_dev_create(fd, "mouse", setup.name) < 0) { close(fd); return -1; }
    return fd;
}

/* ── Vtable implementation ───────────────────────────────────── */

static bool uinput_dimensions_are_valid(const MdInputConfig *cfg) {
    return cfg->screen_width >= MD_INPUT_MIN_SCREEN_DIMENSION &&
           cfg->screen_height >= MD_INPUT_MIN_SCREEN_DIMENSION;
}

static void uinput_destroy(MdInputCtx *ctx);  /* defined below; used on init failure */

static int uinput_init(MdInputCtx *ctx) {
    const MdInputConfig *cfg = &ctx->config;
    if (!uinput_dimensions_are_valid(cfg)) {
        MD_LOG_E("ERROR — screen dimensions must be configured and >= %u (got %ux%u)", MD_INPUT_MIN_SCREEN_DIMENSION, cfg->screen_width, cfg->screen_height);
        return -1;
    }

    UinputState *st = calloc(1, sizeof(UinputState));
    if (!st) return -1;

    st->screen_w = cfg->screen_width;
    st->screen_h = cfg->screen_height;
    st->kbd_fd   = -1;
    st->mouse_fd = -1;

    st->kbd_fd = create_keyboard();
    if (st->kbd_fd < 0)
        MD_LOG_W("WARNING — keyboard device creation failed");

    st->mouse_fd = create_mouse(st->screen_w, st->screen_h);
    if (st->mouse_fd < 0)
        MD_LOG_W("WARNING — mouse device creation failed");

    ctx->backend_data = st;
    ctx->ready = (st->kbd_fd >= 0 || st->mouse_fd >= 0);

    if (!ctx->ready) {
        MD_LOG_E("no virtual devices created — check /dev/uinput permissions");
        uinput_destroy(ctx);
        return -1;
    }

    return 0;
}

static int uinput_mouse_move(MdInputCtx *ctx, int x, int y) {
    UinputState *st = ctx->backend_data;
    if (!st || st->mouse_fd < 0) return -1;

    const int32_t ax = uinput_abs(x, ctx->config.screen_width, st->screen_w);
    const int32_t ay = uinput_abs(y, ctx->config.screen_height, st->screen_h);
    if (emit(st->mouse_fd, EV_ABS, ABS_X, ax) < 0 ||
        emit(st->mouse_fd, EV_ABS, ABS_Y, ay) < 0 ||
        syn(st->mouse_fd) < 0)
        return -1;
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

    if (emit(st->mouse_fd, EV_KEY, btn, pressed ? 1 : 0) < 0 ||
        syn(st->mouse_fd) < 0)
        return -1;
    return 0;
}

static int uinput_mouse_scroll(MdInputCtx *ctx, int dx, int dy) {
    UinputState *st = ctx->backend_data;
    if (!st || st->mouse_fd < 0) return -1;

    if (dy != 0 && emit(st->mouse_fd, EV_REL, REL_WHEEL, dy) < 0)
        return -1;
    if (dx != 0 && emit(st->mouse_fd, EV_REL, REL_HWHEEL, dx) < 0)
        return -1;
    if (syn(st->mouse_fd) < 0)
        return -1;
    return 0;
}

static int uinput_key_event(MdInputCtx *ctx, uint32_t keysym, int pressed) {
    UinputState *st = ctx->backend_data;
    if (!st || st->kbd_fd < 0) return -1;

    /* keysym values in our system are Linux KEY_* codes directly */
    if (emit(st->kbd_fd, EV_KEY, (uint16_t)keysym, pressed ? 1 : 0) < 0 ||
        syn(st->kbd_fd) < 0)
        return -1;
    return 0;
}

static int uinput_type_text(MdInputCtx *ctx, const char *utf8) {
    UinputState *st = ctx->backend_data;
    if (!st || st->kbd_fd < 0 || !utf8) return -1;

    const uint16_t shift_sym = KEY_LEFTSHIFT;

    for (const char *p = utf8; *p != '\0'; p++) {
        uint32_t sym;
        bool need_shift;
        if (!md_input_ascii_key(*p, &sym, &need_shift))
            continue; /* non-ASCII and unmapped characters are skipped */

        if (need_shift) {
            if (emit(st->kbd_fd, EV_KEY, shift_sym, 1) < 0 ||
                syn(st->kbd_fd) < 0)
                return -1;
        }

        if (emit(st->kbd_fd, EV_KEY, (uint16_t)sym, 1) < 0 ||
            syn(st->kbd_fd) < 0)
            return -1;
        md_input_settle();
        if (emit(st->kbd_fd, EV_KEY, (uint16_t)sym, 0) < 0 ||
            syn(st->kbd_fd) < 0)
            return -1;

        if (need_shift) {
            if (emit(st->kbd_fd, EV_KEY, shift_sym, 0) < 0 ||
                syn(st->kbd_fd) < 0)
                return -1;
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
