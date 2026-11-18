/* Deterministic wire-level peer: validates requests, not backend internals. */
#include "input_wayland_server.h"
#include "wlr-virtual-pointer-unstable-v1-server-protocol.h"
#include "virtual-keyboard-unstable-v1-server-protocol.h"
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

struct InputTestServer {
    struct wl_display *display;
    struct wl_event_source *stop_source;
    pthread_t thread;
    pthread_mutex_t mutex;
    int stop[2];
    unsigned flags;
    char dir[64], path[96];
    InputTestEvents events;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
};
static void destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void motion(struct wl_client *c, struct wl_resource *r, uint32_t t,
                   wl_fixed_t x, wl_fixed_t y) {
    (void)c; (void)r; (void)t; (void)x; (void)y;
}
static void absolute(struct wl_client *c, struct wl_resource *r, uint32_t t,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)t;
    InputTestServer *s = wl_resource_get_user_data(r);
    if (s->flags & 16) {
        wl_resource_post_error(r, 0, "test disconnect on motion");
        return;
    }
    (void)c;
    pthread_mutex_lock(&s->mutex);
    s->events.moves++; s->events.x = x; s->events.y = y;
    s->events.width = w; s->events.height = h;
    pthread_mutex_unlock(&s->mutex);
}
static void button(struct wl_client *c, struct wl_resource *r, uint32_t t,
                   uint32_t code, uint32_t state) {
    (void)c; (void)t;
    InputTestServer *s = wl_resource_get_user_data(r);
    const uint32_t codes[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    pthread_mutex_lock(&s->mutex);
    for (int i = 0; i < 3; i++) if (code == codes[i]) {
        if (state) s->events.button_down[i]++;
        else s->events.button_up[i]++;
    }
    pthread_mutex_unlock(&s->mutex);
}
static void axis(struct wl_client *c, struct wl_resource *r, uint32_t t,
                 uint32_t a, wl_fixed_t v) {
    (void)c; (void)r; (void)t; (void)a; (void)v;
}
static void frame(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    InputTestServer *s = wl_resource_get_user_data(r);
    pthread_mutex_lock(&s->mutex); s->events.frames++; pthread_mutex_unlock(&s->mutex);
}
static void axis_source(struct wl_client *c, struct wl_resource *r, uint32_t a) {
    (void)c; (void)r; (void)a;
}
static void axis_stop(struct wl_client *c, struct wl_resource *r, uint32_t t, uint32_t a) {
    (void)c; (void)r; (void)t; (void)a;
}
static void discrete(struct wl_client *c, struct wl_resource *r, uint32_t t,
                     uint32_t a, wl_fixed_t v, int32_t d) {
    (void)c; (void)t; (void)v;
    InputTestServer *s = wl_resource_get_user_data(r);
    pthread_mutex_lock(&s->mutex);
    if (a == WL_POINTER_AXIS_HORIZONTAL_SCROLL) s->events.scroll_x += d;
    else s->events.scroll_y += d;
    pthread_mutex_unlock(&s->mutex);
}
static const struct zwlr_virtual_pointer_v1_interface pointer_impl = {
    .motion = motion, .motion_absolute = absolute, .button = button, .axis = axis,
    .frame = frame, .axis_source = axis_source, .axis_stop = axis_stop,
    .axis_discrete = discrete, .destroy = destroy,
};
static void create_pointer(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *seat, uint32_t id) {
    (void)seat;
    struct wl_resource *p = wl_resource_create(c, &zwlr_virtual_pointer_v1_interface, 1, id);
    wl_resource_set_implementation(p, &pointer_impl, wl_resource_get_user_data(r), NULL);
}
static const struct zwlr_virtual_pointer_manager_v1_interface pointer_manager_impl = {
    .create_virtual_pointer = create_pointer, .destroy = destroy,
};
static void keymap(struct wl_client *c, struct wl_resource *r, uint32_t format,
                   int32_t fd, uint32_t size) {
    (void)c;
    InputTestServer *s = wl_resource_get_user_data(r);
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && map != MAP_FAILED) {
        if (s->state) xkb_state_unref(s->state);
        if (s->keymap) xkb_keymap_unref(s->keymap);
        s->keymap = xkb_keymap_new_from_string(s->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
        s->state = s->keymap ? xkb_state_new(s->keymap) : NULL;
    }
    if (map != MAP_FAILED) munmap(map, size);
    close(fd);
    pthread_mutex_lock(&s->mutex);
    s->events.keymaps++; s->events.keymap_valid = s->state != NULL;
    pthread_mutex_unlock(&s->mutex);
}
static void key(struct wl_client *c, struct wl_resource *r, uint32_t t,
                uint32_t code, uint32_t state) {
    (void)c; (void)t;
    InputTestServer *s = wl_resource_get_user_data(r);
    pthread_mutex_lock(&s->mutex);
    if (state) s->events.key_down++;
    else s->events.key_up++;
    if (s->state) {
        char utf8[16];
        int len = xkb_state_key_get_utf8(s->state, code + 8, utf8, sizeof(utf8));
        char *text = state ? s->events.text : s->events.release_text;
        size_t used = strlen(text);
        if (len > 0 && used + (size_t)len < sizeof(s->events.text))
            memcpy(text + used, utf8, (size_t)len + 1);
    }
    pthread_mutex_unlock(&s->mutex);
}
static void modifiers(struct wl_client *c, struct wl_resource *r, uint32_t d,
                      uint32_t l, uint32_t k, uint32_t g) {
    (void)c;
    InputTestServer *s = wl_resource_get_user_data(r);
    if (!s->state) return;
    xkb_state_update_mask(s->state, d, l, k, 0, 0, g);
    pthread_mutex_lock(&s->mutex);
    s->events.modifier_events++;
    s->events.saw_ctrl |= xkb_state_mod_name_is_active(s->state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_DEPRESSED) > 0;
    s->events.saw_shift |= xkb_state_mod_name_is_active(s->state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_DEPRESSED) > 0;
    pthread_mutex_unlock(&s->mutex);
}
static const struct zwp_virtual_keyboard_v1_interface keyboard_impl = {
    .keymap = keymap, .key = key, .modifiers = modifiers, .destroy = destroy,
};
static void create_keyboard(struct wl_client *c, struct wl_resource *r,
                            struct wl_resource *seat, uint32_t id) {
    (void)seat;
    InputTestServer *s = wl_resource_get_user_data(r);
    if (s->flags & 8) {
        wl_resource_post_error(r, ZWP_VIRTUAL_KEYBOARD_MANAGER_V1_ERROR_UNAUTHORIZED, "denied by test policy");
        return;
    }
    struct wl_resource *k = wl_resource_create(c, &zwp_virtual_keyboard_v1_interface, 1, id);
    wl_resource_set_implementation(k, &keyboard_impl, s, NULL);
}
static const struct zwp_virtual_keyboard_manager_v1_interface keyboard_manager_impl = {
    .create_virtual_keyboard = create_keyboard,
};
static const struct wl_seat_interface seat_impl = {0};
static void bind_seat(struct wl_client *c, void *data, uint32_t v, uint32_t id) {
    (void)v;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, 1, id);
    wl_resource_set_implementation(r, &seat_impl, data, NULL);
}
static void bind_pointer(struct wl_client *c, void *data, uint32_t v, uint32_t id) {
    (void)v;
    struct wl_resource *r = wl_resource_create(c, &zwlr_virtual_pointer_manager_v1_interface, 1, id);
    wl_resource_set_implementation(r, &pointer_manager_impl, data, NULL);
}
static void bind_keyboard(struct wl_client *c, void *data, uint32_t v, uint32_t id) {
    (void)v;
    struct wl_resource *r = wl_resource_create(c, &zwp_virtual_keyboard_manager_v1_interface, 1, id);
    wl_resource_set_implementation(r, &keyboard_manager_impl, data, NULL);
}
static int stop_server(int fd, uint32_t mask, void *data) {
    (void)mask;
    char b;
    if (read(fd, &b, 1) != 1) abort();
    wl_display_terminate(((InputTestServer *)data)->display);
    return 0;
}
static void *run_server(void *data) {
    InputTestServer *s = data;
    if (s->flags & 32) {
        char byte;
        if (read(s->stop[0], &byte, 1) != 1) abort();
    } else wl_display_run(s->display);
    return NULL;
}
InputTestServer *input_test_server_start(unsigned globals) {
    InputTestServer *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->flags = globals;
    strcpy(s->dir, "/tmp/md-input-XXXXXX");
    if (!mkdtemp(s->dir)) { free(s); return NULL; }
    snprintf(s->path, sizeof(s->path), "%s/wayland", s->dir);
    s->display = wl_display_create();
    s->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!s->display || !s->xkb || pipe(s->stop) || pthread_mutex_init(&s->mutex, NULL)) abort();
    if (wl_display_add_socket(s->display, s->path) < 0) abort();
    if (globals & 1) wl_global_create(s->display, &wl_seat_interface, 1, s, bind_seat);
    if (globals & 2) wl_global_create(s->display, &zwlr_virtual_pointer_manager_v1_interface, 1, s, bind_pointer);
    if (globals & 4) wl_global_create(s->display, &zwp_virtual_keyboard_manager_v1_interface, 1, s, bind_keyboard);
    s->stop_source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
        s->stop[0], WL_EVENT_READABLE, stop_server, s);
    if (pthread_create(&s->thread, NULL, run_server, s)) abort();
    return s;
}
const char *input_test_server_path(InputTestServer *s) { return s->path; }
InputTestEvents input_test_server_events(InputTestServer *s) {
    pthread_mutex_lock(&s->mutex);
    InputTestEvents events = s->events;
    pthread_mutex_unlock(&s->mutex);
    return events;
}
void input_test_server_stop(InputTestServer *s) {
    if (!s) return;
    if (write(s->stop[1], "x", 1) != 1) abort();
    pthread_join(s->thread, NULL);
    wl_event_source_remove(s->stop_source);
    wl_display_destroy_clients(s->display);
    wl_display_destroy(s->display);
    close(s->stop[0]); close(s->stop[1]);
    if (s->state) xkb_state_unref(s->state);
    if (s->keymap) xkb_keymap_unref(s->keymap);
    xkb_context_unref(s->xkb);
    pthread_mutex_destroy(&s->mutex);
    rmdir(s->dir);
    free(s);
}
