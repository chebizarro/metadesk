/* End-to-end first-key regression: disposable-VM ONLY.
 * Opt in with MD_INPUT_DESKTOP_BACKEND=uinput; default is SKIP. */
#include "input.h"
#include "md_time.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
    if (!backend || strcmp(backend, "uinput")) return 77;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    Window root = DefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 50, 50, 500, 300, 0, 0, 0xffffff);
    XStoreName(d, w, "metadesk W4 owned input test");
    XSelectInput(d, w, StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                       PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
    XMapRaised(d, w); XFlush(d);
    int rc = 1, cycles = 100;
    if (wait_event(d, w, MapNotify, 0)) goto done;
    for (int i = 0; i < cycles; i++) {
        XSetInputFocus(d, w, RevertToParent, CurrentTime); XSync(d, False);
        MdInputConfig cfg = { .screen_width = DisplayWidth(d, DefaultScreen(d)),
            .screen_height = DisplayHeight(d, DefaultScreen(d)), .backend = backend };
        uint64_t before = md_monotonic_ns();
        MdInput *input = md_input_create(&cfg);
        uint64_t created = md_monotonic_ns();
        if (!input) { fprintf(stderr, "create failed cycle %d\n", i); goto done; }
        uint64_t first = md_monotonic_ns();
        int a = input->vtable->key_event(input, md_input_keysym_from_name("a"), 1);
        int b = input->vtable->key_event(input, md_input_keysym_from_name("a"), 0);
        int keycode = XKeysymToKeycode(d, XK_a);
        bool received = !a && !b && !wait_event(d, w, KeyPress, keycode) &&
                         !wait_event(d, w, KeyRelease, keycode);
        printf("cycle %d create=%.3fms first-write=%.3fms first-key=%s\n", i,
            (created-before)/1e6, (first-created)/1e6, received ? "PASS" : "FAIL");
        fflush(stdout);
        md_input_destroy(input);
        if (!received) goto done;
    }
    printf("PASS: %d/%d first-key press/release cycles (%s)\n", cycles, cycles, backend);
    rc = 0;
done:
    XDestroyWindow(d, w); XCloseDisplay(d);
    return rc;
}
