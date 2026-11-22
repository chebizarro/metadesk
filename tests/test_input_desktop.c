/* End-to-end first-key regression. uinput is disposable-VM ONLY. The portal
 * mode is live-safe: every injected key/button targets this owned window.
 * Opt in with MD_INPUT_DESKTOP_BACKEND=uinput|portal|auto; default is SKIP.
 * auto is permitted here only on Wayland with an available portal. */
#include "input.h"
#include "md_time.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stddef.h>

static int next_event(Display *d, Window w, int type, int detail, XEvent *out) {
    int64_t deadline = md_now_ms() + 2000;
    while (md_now_ms() < deadline) {
        while (XPending(d) && md_now_ms() < deadline) {
            XEvent e; XNextEvent(d, &e);
            if (e.xany.window != w || e.type != type) continue;
            if (detail >= 0 && (type == KeyPress || type == KeyRelease) && (int)e.xkey.keycode != detail) {
                fprintf(stderr, "event %d keycode=%u expected=%d\n", type, e.xkey.keycode, detail); return -1;
            }
            if ((type == ButtonPress || type == ButtonRelease) && (int)e.xbutton.button != detail) {
                fprintf(stderr, "event %d button=%u expected=%d\n", type, e.xbutton.button, detail); return -1;
            }
            if (out) *out = e;
            return 0;
        }
        struct pollfd fd = { ConnectionNumber(d), POLLIN, 0 };
        int64_t left = deadline - md_now_ms();
        if (left <= 0) break;
        int n = poll(&fd, 1, (int)left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (fd.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
    }
    fprintf(stderr, "event timeout type=%d detail=%d\n", type, detail);
    return -1;
}

static int wait_event(Display *d, Window w, int type, int detail) {
    return next_event(d, w, type, detail, NULL);
}

static bool owned_focus(Display *d, Window w) {
    Window focus; int revert;
    XGetInputFocus(d, &focus, &revert);
    return focus == w;
}

static bool owned_pointer(Display *d, Window w) {
    /* Mutter reparents Xwayland clients. The root child is the WM frame,
     * not our client: verify its ancestry AND client-local bounds. */
    Window frame = w, root, parent, *children;
    unsigned count;
    for (;;) {
        if (!XQueryTree(d, frame, &root, &parent, &children, &count)) return false;
        if (children) XFree(children);
        if (parent == root) break;
        if (!parent) return false;
        frame = parent;
    }
    Window pointer_root, child; int rx, ry, x, y; unsigned mask;
    if (!XQueryPointer(d, root, &pointer_root, &child, &rx, &ry, &x, &y, &mask) || child != frame)
        return false;
    XWindowAttributes attrs;
    return XGetWindowAttributes(d, w, &attrs) &&
        XQueryPointer(d, w, &pointer_root, &child, &rx, &ry, &x, &y, &mask) &&
        x >= 0 && y >= 0 && x < attrs.width && y < attrs.height;
}

static bool portal_keyboard(Display *d, Window w, MdInput *input) {
    const char *text = "Metadesk 42!";
    if (!owned_focus(d, w)) { fputs("text: lost owned focus\n", stderr); return false; }
    if (md_input_type_text(input, text)) { fputs("type_text rejected input\n", stderr); return false; }
    char received[64] = {0};
    size_t count = 0;
    /* Shift presses produce no text; compare what the target actually sees,
     * not just the success of the injection calls. */
    while (count < strlen(text)) {
        XEvent event;
        if (next_event(d, w, KeyPress, -1, &event)) return false;
        char chars[8]; KeySym sym;
        int n = XLookupString(&event.xkey, chars, sizeof(chars), &sym, NULL);
        if (n < 0 || count + (size_t)n >= sizeof(received)) return false;
        memcpy(received + count, chars, (size_t)n); count += (size_t)n;
    }
    if (strcmp(received, text)) {
        fprintf(stderr, "text received='%s' expected='%s'\n", received, text); return false;
    }
    /* Drain the final release before testing the modifier sequence. */
    if (wait_event(d, w, KeyRelease, XKeysymToKeycode(d, XK_1)) ||
        wait_event(d, w, KeyRelease, XKeysymToKeycode(d, XK_Shift_L))) return false;
    const char *keys[] = { "ctrl", "a" };
    if (!owned_focus(d, w) || md_input_key_combo(input, keys, 2)) return false;
    int ctrl = XKeysymToKeycode(d, XK_Control_L), a = XKeysymToKeycode(d, XK_a);
    XEvent event;
    if (wait_event(d, w, KeyPress, ctrl) || next_event(d, w, KeyPress, a, &event) ||
        !(event.xkey.state & ControlMask) || wait_event(d, w, KeyRelease, a) ||
        wait_event(d, w, KeyRelease, ctrl)) return false;
    printf("owned-window text='%s' modifier=ControlMask PASS\n", received);
    return true;
}

int main(void) {
    const char *backend = getenv("MD_INPUT_DESKTOP_BACKEND");
    if (!backend || (strcmp(backend, "uinput") && strcmp(backend, "portal") && strcmp(backend, "auto"))) return 77;
    bool portal = strcmp(backend, "uinput") != 0;
    if (!strcmp(backend, "auto")) {
#ifdef MD_HAVE_PORTAL_INPUT
        const char *session = getenv("XDG_SESSION_TYPE");
        if (!session || strcmp(session, "wayland") || !md_input_portal_available()) return 77;
#else
        return 77;
#endif
    }
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
    XStoreName(d, w, "metadesk owned input acceptance");
    XSelectInput(d, w, StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                       PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
    XMapRaised(d, w); XFlush(d);
    int rc = 1, cycles = !strcmp(backend, "uinput") ? 100 : 1;
    if (wait_event(d, w, MapNotify, 0)) goto done;
    for (int i = 0; i < cycles; i++) {
        XSetInputFocus(d, w, RevertToParent, CurrentTime); XSync(d, False);
        MdInputConfig cfg = { .screen_width = DisplayWidth(d, DefaultScreen(d)),
            .screen_height = DisplayHeight(d, DefaultScreen(d)), .backend = backend,
            /* Test coordinates are root-window logical coordinates, including
             * every output. Do not ask the backend to guess one EI region. */
            .output_width = DisplayWidth(d, DefaultScreen(d)),
            .output_height = DisplayHeight(d, DefaultScreen(d)),
            /* Human-operated consent watchdog, not an input-readiness delay. */
            .portal_timeout_ms = 180000 };
        uint64_t before = md_monotonic_ns();
        MdInput *input = md_input_create(&cfg);
        uint64_t created = md_monotonic_ns();
        if (!input) { fprintf(stderr, "create failed cycle %d\n", i); goto done; }
        /* Portal consent may change focus. Never inject until the server
         * confirms our owned window is focused. uinput has no extra roundtrip
         * after create, so its first write tests the actual readiness contract. */
        if (portal) { XSetInputFocus(d, w, RevertToParent, CurrentTime); XSync(d, False); }
        if (portal) {
            if (!owned_focus(d, w)) { md_input_destroy(input); goto done; }
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
        if (received && portal) {
            received = portal_keyboard(d, w, input);
            Window child; int rx, ry, wx, wy; unsigned mask;
            XRaiseWindow(d, w); XSync(d, False);
            XTranslateCoordinates(d, w, root, 100, 80, &rx, &ry, &child);
            Window pointer_root, pointer_child; int px, py;
            if (!XQueryPointer(d, root, &pointer_root, &pointer_child, &px, &py, &wx, &wy, &mask)) received = false;
            if (received && px == rx && py == ry) rx++;
            if (received && (md_input_mouse_move(input, rx, ry) || wait_event(d, w, MotionNotify, 0))) {
                fputs("pointer move failed\n", stderr); received = false;
            }
            XQueryPointer(d, root, &pointer_root, &pointer_child, &px, &py, &wx, &wy, &mask);
            /* Do not click through an occluding window or a wrong mapping. */
            if (!owned_pointer(d, w) || abs(px-rx) > 1 || abs(py-ry) > 1) {
                fprintf(stderr, "pointer target=(%d,%d) observed=(%d,%d) child=%lu owned=%lu\n",
                        rx, ry, px, py, pointer_child, w); received = false;
            }
            const int buttons[] = { 1, 3, 2 };
            for (int button = 0; received && button < 3; button++) {
                XQueryPointer(d, root, &pointer_root, &pointer_child, &px, &py, &wx, &wy, &mask);
                if (!owned_pointer(d, w) || input->vtable->mouse_button(input, button, 1) ||
                    input->vtable->mouse_button(input, button, 0) ||
                    wait_event(d, w, ButtonPress, buttons[button]) ||
                    wait_event(d, w, ButtonRelease, buttons[button])) received = false;
            }
            if (received && (!owned_pointer(d, w) || md_input_scroll(input, 0, 1) ||
                             wait_event(d, w, ButtonPress, 4) || wait_event(d, w, ButtonRelease, 4))) received = false;
            if (received && (!owned_pointer(d, w) || md_input_scroll(input, 1, 0) ||
                             wait_event(d, w, ButtonPress, 7) || wait_event(d, w, ButtonRelease, 7))) received = false;
            printf("owned-window portal pointer/button/scroll=%s\n", received ? "PASS" : "FAIL");
            if (received && getenv("MD_INPUT_DESKTOP_REVOKE")) {
                puts("REVOKE READY: stop this RemoteDesktop session using GNOME's sharing indicator, then send a newline on stdin");
                fflush(stdout);
                if (getchar() == EOF || md_input_is_ready(input) ||
                    input->vtable->key_event(input, md_input_keysym_from_name("a"), 1) == 0)
                    received = false;
                printf("portal revocation fails closed=%s\n", received ? "PASS" : "FAIL");
            }
        }
        md_input_destroy(input);
        puts("input session destroyed"); fflush(stdout);
        if (!received) goto done;
    }
    printf("PASS: %d/%d first-key press/release cycles (%s)\n", cycles, cycles, backend);
    rc = 0;
done:
    XDestroyWindow(d, w); XCloseDisplay(d); close(guard);
    return rc;
}
