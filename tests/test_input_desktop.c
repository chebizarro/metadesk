/* End-to-end first-key regression. uinput is disposable-VM ONLY. The portal
 * mode is live-safe: every injected key/button targets this owned window.
 * Opt in with MD_INPUT_DESKTOP_BACKEND=uinput|portal; default is SKIP. */
#include "input.h"
#include "md_time.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stddef.h>

static int wait_event(Display *d, Window w, int type, int detail) {
    int64_t deadline = md_now_ms() + 2000;
    while (md_now_ms() < deadline) {
        while (XPending(d) && md_now_ms() < deadline) {
            XEvent e; XNextEvent(d, &e);
            if (e.xany.window != w || e.type != type) continue;
            if ((type == KeyPress || type == KeyRelease) && (int)e.xkey.keycode != detail) return -1;
            if ((type == ButtonPress || type == ButtonRelease) && (int)e.xbutton.button != detail) return -1;
            return 0;
        }
        struct pollfd fd = { ConnectionNumber(d), POLLIN, 0 };
        int64_t left = deadline - md_now_ms();
        if (left <= 0) break;
        int n = poll(&fd, 1, (int)left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (fd.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
    }
    return -1;
}

int main(void) {
    const char *backend = getenv("MD_INPUT_DESKTOP_BACKEND");
    if (!backend || (strcmp(backend, "uinput") && strcmp(backend, "portal"))) return 77;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    /* Focus is shared by this desktop. Fail rather than run two observers
     * against each other (e.g. normal and sanitizer acceptance in parallel).
     * Linux abstract sockets leave no test lock files behind. */
    int guard = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int len = snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
        "metadesk-input-desktop-%lu-%s", (unsigned long)getuid(), DisplayString(d));
    if (guard < 0 || len < 0 || len >= (int)sizeof(address.sun_path) - 1 ||
        bind(guard, (struct sockaddr *)&address, offsetof(struct sockaddr_un, sun_path) + 1 + len)) {
        fputs("owned-window input test already active, or cannot acquire desktop guard\n", stderr);
        if (guard >= 0) close(guard);
        XCloseDisplay(d); return 1;
    }
    Window root = DefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 50, 50, 500, 300, 0, 0, 0xffffff);
    XStoreName(d, w, "metadesk W4 owned input test");
    XSelectInput(d, w, StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                       PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
    XMapRaised(d, w); XFlush(d);
    int rc = 1, cycles = !strcmp(backend, "uinput") ? 100 : 1;
    if (wait_event(d, w, MapNotify, 0)) goto done;
    for (int i = 0; i < cycles; i++) {
        XSetInputFocus(d, w, RevertToParent, CurrentTime); XSync(d, False);
        MdInputConfig cfg = { .screen_width = DisplayWidth(d, DefaultScreen(d)),
            .screen_height = DisplayHeight(d, DefaultScreen(d)), .backend = backend,
            .portal_timeout_ms = 30000 };
        uint64_t before = md_monotonic_ns();
        MdInput *input = md_input_create(&cfg);
        uint64_t created = md_monotonic_ns();
        if (!input) { fprintf(stderr, "create failed cycle %d\n", i); goto done; }
        /* Portal consent may change focus. Never inject until the server
         * confirms our owned window is focused. uinput has no extra roundtrip
         * after create, so its first write tests the actual readiness contract. */
        if (!strcmp(backend, "portal")) { XSetInputFocus(d, w, RevertToParent, CurrentTime); XSync(d, False); }
        Window focus; int revert;
        if (!strcmp(backend, "portal")) {
            XGetInputFocus(d, &focus, &revert);
            if (focus != w) { md_input_destroy(input); goto done; }
        }
        uint64_t first = md_monotonic_ns();
        int a = input->vtable->key_event(input, md_input_keysym_from_name("a"), 1);
        int b = input->vtable->key_event(input, md_input_keysym_from_name("a"), 0);
        int keycode = XKeysymToKeycode(d, XK_a);
        bool received = !a && !b && !wait_event(d, w, KeyPress, keycode) &&
                         !wait_event(d, w, KeyRelease, keycode);
        printf("cycle %d create=%.3fms first-write=%.3fms first-key=%s\n", i,
            (created-before)/1e6, (first-created)/1e6, received ? "PASS" : "FAIL");
        fflush(stdout);
        if (received && !strcmp(backend, "portal")) {
            Window child; int rx, ry, wx, wy; unsigned mask;
            XRaiseWindow(d, w); XSync(d, False);
            XTranslateCoordinates(d, w, root, 100, 80, &rx, &ry, &child);
            Window pointer_root, pointer_child; int px, py;
            if (!XQueryPointer(d, root, &pointer_root, &pointer_child, &px, &py, &wx, &wy, &mask)) received = false;
            if (received && px == rx && py == ry) rx++;
            if (received && (md_input_mouse_move(input, rx, ry) || wait_event(d, w, MotionNotify, 0))) received = false;
            XQueryPointer(d, root, &pointer_root, &pointer_child, &px, &py, &wx, &wy, &mask);
            /* Do not click through an occluding window or a wrong mapping. */
            if (pointer_child != w || abs(px-rx) > 1 || abs(py-ry) > 1) received = false;
            if (received && (input->vtable->mouse_button(input, 0, 1) ||
                             input->vtable->mouse_button(input, 0, 0) ||
                             wait_event(d, w, ButtonPress, 1) || wait_event(d, w, ButtonRelease, 1))) received = false;
            if (received && (md_input_scroll(input, 0, 1) ||
                             wait_event(d, w, ButtonPress, 4) || wait_event(d, w, ButtonRelease, 4))) received = false;
            printf("owned-window portal pointer/button/scroll=%s\n", received ? "PASS" : "FAIL");
        }
        md_input_destroy(input);
        if (!received) goto done;
    }
    printf("PASS: %d/%d first-key press/release cycles (%s)\n", cycles, cycles, backend);
    rc = 0;
done:
    XDestroyWindow(d, w); XCloseDisplay(d); close(guard);
    return rc;
}
