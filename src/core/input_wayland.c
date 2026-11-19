/* Explicit, session-local wlroots virtual input. No ambient display or uinput. */
#include "input.h"
#include "log.h"
#define MD_LOG_TAG "input_wayland"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IO_TIMEOUT_MS 2000
#define TEXT_LIMIT 4096

typedef struct {
    pthread_mutex_t mutex;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
    struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
    struct zwlr_virtual_pointer_v1 *pointer;
    struct zwp_virtual_keyboard_v1 *keyboard;
    uint32_t seat_id, pointer_id, keyboard_id;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    xkb_mod_mask_t shift_mask;
    uint32_t held[KEY_MAX + 1];
    bool shifted[KEY_MAX + 1], buttons[3], removed;
} WaylandInput;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void sync_done(void *data, struct wl_callback *cb, uint32_t serial) {
    (void)cb; (void)serial;
    *(bool *)data = true;
}
static const struct wl_callback_listener sync_listener = { .done = sync_done };

/* Protocol barrier, not a sleep. Own all display I/O under the context mutex
 * and bound an unresponsive compositor, including a full outgoing socket. */
static int sync_display(WaylandInput *s) {
    bool done = false;
    struct wl_callback *cb = wl_display_sync(s->display);
    if (!cb) return -1;
    wl_callback_add_listener(cb, &sync_listener, &done);
    int result = -1;
    int64_t deadline = now_ms() + IO_TIMEOUT_MS;
    while (!done) {
        if (wl_display_dispatch_pending(s->display) < 0) break;
        if (done) { result = 0; break; }
        if (wl_display_prepare_read(s->display) != 0) continue;
        int flushed = wl_display_flush(s->display);
        if (flushed < 0 && errno != EAGAIN) {
            wl_display_cancel_read(s->display);
            break;
        }
        struct pollfd fd = { .fd = wl_display_get_fd(s->display),
            .events = POLLIN | (flushed < 0 ? POLLOUT : 0) };
        int64_t remaining = deadline - now_ms();
        int rc = remaining > 0 ? poll(&fd, 1, (int)remaining) : 0;
        if (rc < 0 && errno == EINTR) {
            wl_display_cancel_read(s->display);
            continue;
        }
        if (rc <= 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            wl_display_cancel_read(s->display);
            break;
        }
        if (fd.revents & POLLIN) {
            if (wl_display_read_events(s->display) < 0) break;
        } else {
            wl_display_cancel_read(s->display);
        }
    }
    wl_callback_destroy(cb);
    return result == 0 && !s->removed ? 0 : -1;
}
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data; (void)seat; (void)caps;
}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities };
static void global(void *data, struct wl_registry *registry, uint32_t id,
                   const char *interface, uint32_t version) {
    WaylandInput *s = data;
    if (!version) return;
    if (!strcmp(interface, wl_seat_interface.name) && !s->seat) {
        s->seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
        s->seat_id = id;
        if (s->seat) wl_seat_add_listener(s->seat, &seat_listener, s);
    } else if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name)
               && !s->pointer_manager) {
        s->pointer_manager = wl_registry_bind(registry, id,
            &zwlr_virtual_pointer_manager_v1_interface, 1);
        s->pointer_id = id;
    } else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name)
               && !s->keyboard_manager) {
        s->keyboard_manager = wl_registry_bind(registry, id,
            &zwp_virtual_keyboard_manager_v1_interface, 1);
        s->keyboard_id = id;
    }
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t id) {
    (void)registry;
    WaylandInput *s = data;
    if (id == s->seat_id || id == s->pointer_id || id == s->keyboard_id)
        s->removed = true;
}
static const struct wl_registry_listener registry_listener = {
    .global = global, .global_remove = global_remove,
};
static void send_modifiers(WaylandInput *s) {
    xkb_mod_mask_t depressed = xkb_state_serialize_mods(s->state, XKB_STATE_MODS_DEPRESSED);
    for (unsigned i = 0; i <= KEY_MAX; i++)
        if (s->shifted[i]) depressed |= s->shift_mask;
    zwp_virtual_keyboard_v1_modifiers(s->keyboard, depressed,
        xkb_state_serialize_mods(s->state, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(s->state, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(s->state, XKB_STATE_LAYOUT_EFFECTIVE));
}
static bool lookup(WaylandInput *s, uint32_t sym, uint32_t *code, bool *shift) {
    /* Only base/Shift levels of our fixed US layout are supported. */
    for (unsigned level = 0; level < 2; level++) {
        for (xkb_keycode_t k = xkb_keymap_min_keycode(s->keymap);
             k <= xkb_keymap_max_keycode(s->keymap) && k <= KEY_MAX + 8; k++) {
            const xkb_keysym_t *syms;
            if (k < 8 || level >= xkb_keymap_num_levels_for_key(s->keymap, k, 0)) continue;
            int n = xkb_keymap_key_get_syms_by_level(s->keymap, k, 0, level, &syms);
            if (n == 1 && syms[0] == sym) {
                *code = k - 8; *shift = level == 1;
                return true;
            }
        }
    }
    return false;
}
static void emit_key(WaylandInput *s, uint32_t code, uint32_t sym, bool shift, int pressed) {
    /* Release the character with the same synthetic modifiers as its press.
     * Only then release Shift (and update explicitly held modifier keys). */
    if (!pressed)
        zwp_virtual_keyboard_v1_key(s->keyboard, (uint32_t)now_ms(), code,
                                    WL_KEYBOARD_KEY_STATE_RELEASED);
    s->held[code] = pressed ? sym : 0;
    s->shifted[code] = pressed && shift;
    xkb_state_update_key(s->state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    send_modifiers(s);
    if (pressed)
        zwp_virtual_keyboard_v1_key(s->keyboard, (uint32_t)now_ms(), code,
                                    WL_KEYBOARD_KEY_STATE_PRESSED);
}
static const uint32_t button_codes[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
static void release_all(WaylandInput *s) {
    if (s->keyboard && s->state) {
        for (unsigned i = 0; i <= KEY_MAX; i++)
            if (s->held[i]) emit_key(s, i, 0, false, 0);
        zwp_virtual_keyboard_v1_modifiers(s->keyboard, 0, 0, 0, 0);
    }
    if (s->pointer) {
        for (unsigned i = 0; i < 3; i++)
            if (s->buttons[i]) zwlr_virtual_pointer_v1_button(s->pointer,
                (uint32_t)now_ms(), button_codes[i], WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(s->pointer);
    }
}
static void wayland_destroy(MdInputCtx *ctx) {
    WaylandInput *s = ctx->backend_data;
    if (!s) return;
    pthread_mutex_lock(&s->mutex);
    if (ctx->ready && s->display) {
        release_all(s);
        sync_display(s); /* best effort; disconnect destroys virtual devices too */
    }
    ctx->ready = false;
    if (s->keyboard) zwp_virtual_keyboard_v1_destroy(s->keyboard);
    if (s->pointer) zwlr_virtual_pointer_v1_destroy(s->pointer);
    if (s->keyboard_manager) zwp_virtual_keyboard_manager_v1_destroy(s->keyboard_manager);
    if (s->pointer_manager) zwlr_virtual_pointer_manager_v1_destroy(s->pointer_manager);
    if (s->seat) wl_seat_destroy(s->seat);
    if (s->registry) wl_registry_destroy(s->registry);
    if (s->display) {
        wl_display_flush(s->display);
        wl_display_disconnect(s->display);
    }
    if (s->state) xkb_state_unref(s->state);
    if (s->keymap) xkb_keymap_unref(s->keymap);
    if (s->xkb) xkb_context_unref(s->xkb);
    pthread_mutex_unlock(&s->mutex);
    pthread_mutex_destroy(&s->mutex);
    free(s);
    ctx->backend_data = NULL;
}
static int connect_socket(const char *path) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (!path || path[0] != '/' || strlen(path) >= sizeof(addr.sun_path)) return -1;
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) { close(fd); return -1; }
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int error = 0;
        socklen_t len = sizeof(error);
        if (poll(&p, 1, IO_TIMEOUT_MS) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error) {
            close(fd); return -1;
        }
    }
    return fd;
}
static int wayland_init(MdInputCtx *ctx) {
    WaylandInput *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    if (pthread_mutex_init(&s->mutex, NULL)) { free(s); return -1; }
    ctx->backend_data = s;
    int fd = connect_socket(ctx->config.wayland_display);
    if (fd < 0) goto fail;
    s->display = wl_display_connect_to_fd(fd); /* takes fd even on failure */
    if (!s->display) goto fail;
    s->registry = wl_display_get_registry(s->display);
    if (!s->registry) goto fail;
    wl_registry_add_listener(s->registry, &registry_listener, s);
    if (sync_display(s) < 0 || !s->seat || !s->pointer_manager || !s->keyboard_manager)
        goto fail;
    s->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(s->pointer_manager, s->seat);
    s->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(s->keyboard_manager, s->seat);
    s->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!s->pointer || !s->keyboard || !s->xkb) goto fail;
    struct xkb_rule_names names = { .rules = "evdev", .model = "pc105", .layout = "us",
                                   .variant = "", .options = "" };
    s->keymap = xkb_keymap_new_from_names(s->xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!s->keymap) goto fail;
    s->state = xkb_state_new(s->keymap);
    if (!s->state) goto fail;
    xkb_mod_index_t shift = xkb_keymap_mod_get_index(s->keymap, XKB_MOD_NAME_SHIFT);
    if (shift == XKB_MOD_INVALID || shift >= 32) goto fail;
    s->shift_mask = (xkb_mod_mask_t)1 << shift;
    char *map = xkb_keymap_get_as_string(s->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!map) goto fail;
    size_t size = strlen(map) + 1;
    fd = memfd_create("metadesk-input-keymap", MFD_CLOEXEC);
    if (fd < 0) { free(map); goto fail; }
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, map + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += (size_t)n;
    }
    free(map);
    if (written != size) { close(fd); goto fail; }
    zwp_virtual_keyboard_v1_keymap(s->keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    close(fd);
    if (sync_display(s) < 0) goto fail;
    ctx->ready = true;
    return 0;
fail:
    MD_LOG_E("Wayland input unavailable: explicit socket, seat and virtual input protocols required; no fallback");
    wayland_destroy(ctx);
    return -1;
}
/* Failed I/O permanently disables this context; reconnection must be explicit. */
static int finish(MdInputCtx *ctx) {
    WaylandInput *s = ctx->backend_data;
    if (sync_display(s) == 0) return 0;
    ctx->ready = false;
    shutdown(wl_display_get_fd(s->display), SHUT_RDWR);
    return -1;
}
static int wayland_move(MdInputCtx *ctx, int x, int y) {
    WaylandInput *s = ctx->backend_data;
    pthread_mutex_lock(&s->mutex);
    int rc = -1;
    if (ctx->ready) {
        uint32_t w = ctx->config.screen_width, h = ctx->config.screen_height;
        uint32_t px = x < 0 ? 0 : (uint32_t)x >= w ? w - 1 : (uint32_t)x;
        uint32_t py = y < 0 ? 0 : (uint32_t)y >= h ? h - 1 : (uint32_t)y;
        zwlr_virtual_pointer_v1_motion_absolute(s->pointer, (uint32_t)now_ms(), px, py, w, h);
        zwlr_virtual_pointer_v1_frame(s->pointer);
        rc = finish(ctx);
    }
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
static int wayland_button(MdInputCtx *ctx, int button, int pressed) {
    if (button < 0 || button > 2 || (pressed != 0 && pressed != 1)) return -1;
    WaylandInput *s = ctx->backend_data;
    pthread_mutex_lock(&s->mutex);
    int rc = -1;
    if (ctx->ready) {
        s->buttons[button] = pressed;
        zwlr_virtual_pointer_v1_button(s->pointer, (uint32_t)now_ms(), button_codes[button], pressed);
        zwlr_virtual_pointer_v1_frame(s->pointer);
        rc = finish(ctx);
    }
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
static int wayland_scroll(MdInputCtx *ctx, int dx, int dy) {
    /* wl_fixed_t has 24 integer bits; refuse overflow before conversion. */
    if (dx < -838860 || dx > 838860 || dy < -838860 || dy > 838860) return -1;
    WaylandInput *s = ctx->backend_data;
    pthread_mutex_lock(&s->mutex);
    int rc = -1;
    if (ctx->ready) {
        zwlr_virtual_pointer_v1_axis_source(s->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
        /* HAL uses evdev wheel detents: positive right/up. Wayland's
         * vertical axis is positive down, so invert only Y at this boundary. */
        if (dx) zwlr_virtual_pointer_v1_axis_discrete(s->pointer, (uint32_t)now_ms(),
            WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double((double)dx * 10), dx);
        if (dy) zwlr_virtual_pointer_v1_axis_discrete(s->pointer, (uint32_t)now_ms(),
            WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double((double)-dy * 10), -dy);
        zwlr_virtual_pointer_v1_frame(s->pointer);
        rc = finish(ctx);
    }
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
static int wayland_key(MdInputCtx *ctx, uint32_t sym, int pressed) {
    if (pressed != 0 && pressed != 1) return -1;
    WaylandInput *s = ctx->backend_data;
    pthread_mutex_lock(&s->mutex);
    int rc = -1;
    /* The HAL's historical "keysym" space is evdev KEY_*, not XKB keysyms.
     * Only text synthesis does the XKB symbol-to-keycode lookup above. */
    if (ctx->ready && sym > 0 && sym <= KEY_MAX &&
        xkb_keymap_num_layouts_for_key(s->keymap, sym + 8) > 0) {
        if (!!pressed != !!s->held[sym]) emit_key(s, sym, sym, false, pressed);
        rc = finish(ctx);
    }
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
static uint32_t text_sym(unsigned char c) {
    if (c == '\n') return XKB_KEY_Return;
    if (c == '\t') return XKB_KEY_Tab;
    return c >= 32 && c <= 126 ? c : 0;
}
static int wayland_text(MdInputCtx *ctx, const char *text) {
    WaylandInput *s = ctx->backend_data;
    pthread_mutex_lock(&s->mutex);
    int rc = -1;
    size_t len = strnlen(text, TEXT_LIMIT + 1);
    if (!ctx->ready || len > TEXT_LIMIT ||
        xkb_state_serialize_mods(s->state, XKB_STATE_MODS_LOCKED | XKB_STATE_MODS_LATCHED)) goto out;
    for (unsigned i = 0; i <= KEY_MAX; i++) if (s->held[i]) goto out;
    /* Preflight the entire string; do not silently discard a UTF-8 suffix. */
    for (size_t i = 0; i < len; i++) {
        uint32_t code;
        bool shift;
        if (!text_sym((unsigned char)text[i]) ||
            !lookup(s, text_sym((unsigned char)text[i]), &code, &shift)) goto out;
    }
    for (size_t i = 0; i < len; i++) {
        uint32_t code = 0, sym = text_sym((unsigned char)text[i]);
        bool shift = false;
        lookup(s, sym, &code, &shift);
        emit_key(s, code, sym, shift, 1);
        emit_key(s, code, sym, shift, 0);
    }
    rc = finish(ctx);
out:
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
static bool wayland_ready(const MdInputCtx *ctx) {
    WaylandInput *s = ctx->backend_data;
    pthread_mutex_lock(&s->mutex);
    bool ready = ctx->ready;
    pthread_mutex_unlock(&s->mutex);
    return ready;
}
static const MdInputBackend backend = {
    .init = wayland_init, .mouse_move = wayland_move, .mouse_button = wayland_button,
    .mouse_scroll = wayland_scroll, .key_event = wayland_key,
    .type_text = wayland_text, .destroy = wayland_destroy, .is_ready = wayland_ready,
};
const MdInputBackend *md_input_wayland_backend(void) { return &backend; }
