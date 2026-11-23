/* Private-bus RemoteDesktop fixture + actual libeis peer. No desktop, device
 * node or uinput is touched. Test the public factory and wire protocol. */
#include "input.h"
#include "capture.h"
#include "portal_session.h"
#include <sys/socket.h>
#include "md_time.h"
#include "md_test.h"
#include <dbus/dbus.h>
#include <libeis.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-names.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#define PORTAL "org.freedesktop.portal.Desktop"
#define OBJECT "/org/freedesktop/portal/desktop"
#define REMOTE "org.freedesktop.portal.RemoteDesktop"
#define CAST "org.freedesktop.portal.ScreenCast"
#define REQUEST "org.freedesktop.portal.Request"
#define SESSION "org.freedesktop.portal.Session"

typedef struct {
    atomic_int keys, shifts, ms, ones, buttons, moves, scrolls, closes, requests;
    atomic_int starts, opens, eis_connections, session_closes, bad_calls, restore_matches;
} Stats;
static Stats *stats;

static void dict_value(DBusMessageIter *dict, const char *key, int type, const void *v) {
    DBusMessageIter e, variant;
    char sig[2] = {(char)type, 0};
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, sig, &variant);
    dbus_message_iter_append_basic(&variant, type, v);
    dbus_message_iter_close_container(&e, &variant);
    dbus_message_iter_close_container(dict, &e);
}

static bool find_option(DBusMessage *msg, const char *key, DBusMessageIter *value) {
    DBusMessageIter args, array;
    dbus_message_iter_init(msg, &args);
    while (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        if (!dbus_message_iter_next(&args)) return false;
    }
    dbus_message_iter_recurse(&args, &array);
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e, v; const char *name;
        dbus_message_iter_recurse(&array, &e); dbus_message_iter_get_basic(&e, &name);
        dbus_message_iter_next(&e); dbus_message_iter_recurse(&e, &v);
        if (!strcmp(name, key)) { *value = v; return true; }
        dbus_message_iter_next(&array);
    }
    return false;
}

static const char *get_option(DBusMessage *msg, const char *key) {
    DBusMessageIter v;
    const char *value = NULL;
    if (find_option(msg, key, &v) && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(&v, &value);
    return value;
}

static void send_message(DBusConnection *bus, DBusMessage *msg) {
    dbus_connection_send(bus, msg, NULL); dbus_message_unref(msg);
    dbus_connection_flush(bus); /* local controlled fixture */
}

static struct eis_device *add_keyboard(struct eis_seat *seat, int mode) {
    struct eis_device *dev = eis_seat_new_device(seat);
    eis_device_configure_name(dev, "fixture keyboard");
    eis_device_configure_type(dev, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_KEYBOARD);
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    const char *remapped = mode == 6 ?
        "xkb_keymap { xkb_keycodes { include \"evdev+aliases(qwerty)\" }; "
        "xkb_types { include \"complete\" }; xkb_compatibility { include \"complete\" }; "
        "xkb_symbols { include \"pc+us+inet(evdev)\" "
        "replace key <LFSH> { [ Control_L ] }; modifier_map None { <LFSH> }; "
        "modifier_map Control { <LFSH> }; }; };" :
        "xkb_keymap { xkb_keycodes { include \"evdev+aliases(qwerty)\" }; "
        "xkb_types { include \"complete\" }; xkb_compatibility { include \"complete\" }; "
        "xkb_symbols { include \"pc+us+inet(evdev)\" "
        "replace key <LFSH> { type=\"ONE_LEVEL\", symbols[Group1]=[ Shift_L ], "
        "actions[Group1]=[ SetMods(modifiers=Shift+Mod1) ] }; }; };";
    bool remap = mode == 6 || mode == 7;
    struct xkb_keymap *map = remap ?
        xkb_keymap_new_from_string(ctx, remapped, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) :
        xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map) _exit(2);
    if (remap) {
        struct xkb_state *state = xkb_state_new(map);
        xkb_state_update_key(state, 42 + 8, XKB_KEY_DOWN);
        /* Ensure this fixture really exercises the intended unsafe mapping. */
        if (!xkb_state_mod_name_is_active(state, mode == 6 ? XKB_MOD_NAME_CTRL : XKB_MOD_NAME_ALT,
                                          XKB_STATE_MODS_EFFECTIVE) ||
            (mode == 7 && !xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT,
                                                        XKB_STATE_MODS_EFFECTIVE))) {
            fputs("fixture keymap does not activate the required remapped modifiers\n", stderr);
            _exit(2);
        }
        xkb_state_unref(state);
    }
    char *text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    size_t size = strlen(text) + (mode == 5 ? 0 : 1);
    if (write(fd, text, size) != (ssize_t)size) _exit(2);
    struct eis_keymap *keymap = eis_device_new_keymap(dev, EIS_KEYMAP_TYPE_XKB, fd, size);
    eis_keymap_add(keymap); eis_keymap_unref(keymap); close(fd);
    free(text); xkb_keymap_unref(map); xkb_context_unref(ctx);
    eis_device_add(dev); eis_device_resume(dev); return dev;
}

static struct eis_device *add_pointer(struct eis_seat *seat, int mode, bool decoy) {
    struct eis_device *dev = eis_seat_new_device(seat);
    eis_device_configure_name(dev, decoy ? "unrelated pointer" : "fixture pointer");
    eis_device_configure_type(dev, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_capability(dev, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
    if (!decoy) {
        eis_device_configure_capability(dev, EIS_DEVICE_CAP_BUTTON);
        eis_device_configure_capability(dev, EIS_DEVICE_CAP_SCROLL);
    }
    struct eis_region *region = eis_device_new_region(dev);
    if (mode >= 8 && !decoy) {
        eis_region_set_size(region, 960, 540);
        eis_region_set_offset(region, 1200, 400);
        eis_region_set_physical_scale(region, 2.0);
        eis_region_set_mapping_id(region, mode == 13 ? "mismatch" : "fixture-monitor");
    } else {
        eis_region_set_size(region, 1920, 1080);
        if (decoy) eis_region_set_mapping_id(region, mode == 14 ? "fixture-monitor" : "unrelated-monitor");
    }
    eis_region_add(region); eis_region_unref(region);
    eis_device_add(dev); eis_device_resume(dev);
    return dev;
}

static void server(int commands, int notices, int mode) {
    DBusError err = DBUS_ERROR_INIT;
    DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!bus || dbus_bus_request_name(bus, PORTAL, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) _exit(2);
    DBusConnection *replacement_owner = NULL;
    struct eis *eis = eis_new(NULL);
    eis_setup_backend_fd(eis);
    int busfd; dbus_connection_get_unix_fd(bus, &busfd);
    char session[512] = {0};
    int remotes[4], remote_count = 0;
    struct eis_device *pointer = NULL, *keyboard = NULL, *decoy = NULL;
    int step = 0;
    struct eis_seat *owned_seat = NULL;
    char ack = 'R'; if (write(notices, &ack, 1) != 1) _exit(2);
    for (;;) {
        dbus_connection_read_write(bus, 0);
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(bus))) {
            const char *method = dbus_message_get_member(msg);
            if (mode >= 8 && method && (dbus_message_has_interface(msg, REMOTE) ||
                                       dbus_message_has_interface(msg, CAST))) {
                const char *expected[] = {"CreateSession", "SelectDevices", "SelectSources", "Start",
                                          "OpenPipeWireRemote", "ConnectToEIS", "OpenPipeWireRemote"};
                bool cast = step == 2 || step == 4 || step == 6;
                bool valid = step < 7 && !strcmp(method, expected[step]) &&
                    dbus_message_has_interface(msg, cast ? CAST : REMOTE);
                if (step > 0) {
                    DBusMessageIter args;
                    const char *path = NULL;
                    if (dbus_message_iter_init(msg, &args) && dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_OBJECT_PATH)
                        dbus_message_iter_get_basic(&args, &path);
                    valid = valid && path && !strcmp(path, session);
                }
                if (!valid) atomic_fetch_add(&stats->bad_calls, 1);
                step++;
            }
            if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "Get")) {
                DBusMessage *reply = dbus_message_new_method_return(msg);
                DBusMessageIter args, variant; uint32_t version = 2;
                dbus_message_iter_init_append(reply, &args);
                dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "u", &variant);
                dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &version);
                dbus_message_iter_close_container(&args, &variant); send_message(bus, reply);
            } else if (dbus_message_is_method_call(msg, CAST, "OpenPipeWireRemote")) {
                int pair[2];
                if (remote_count == 4 || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) _exit(2);
                remotes[remote_count++] = pair[1];
                atomic_fetch_add(&stats->opens, 1);
                DBusMessage *reply = dbus_message_new_method_return(msg);
                dbus_message_append_args(reply, DBUS_TYPE_UNIX_FD, &pair[0], DBUS_TYPE_INVALID);
                send_message(bus, reply); close(pair[0]);
            } else if (dbus_message_has_interface(msg, REMOTE) ||
                       dbus_message_is_method_call(msg, CAST, "SelectSources")) {
                if (!strcmp(method, "ConnectToEIS")) {
                    atomic_fetch_add(&stats->eis_connections, 1);
                    int fd = eis_backend_fd_add_client(eis);
                    DBusMessage *reply = dbus_message_new_method_return(msg);
                    dbus_message_append_args(reply, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
                    send_message(bus, reply); close(fd);
                } else {
                    atomic_fetch_add(&stats->requests, 1);
                    const char *token = get_option(msg, "handle_token");
                    const char *sender = dbus_message_get_sender(msg);
                    if (!token || !sender) _exit(2);
                    char munged[256], path[512];
                    snprintf(munged, sizeof(munged), "%s", sender+1);
                    for (char *c = munged; *c; c++) if (*c == '.') *c = '_';
                    snprintf(path, sizeof(path), OBJECT "/request/%s/%s", munged, token);
                    if (!strcmp(method, "CreateSession")) {
                        const char *st = get_option(msg, "session_handle_token");
                        snprintf(session, sizeof(session), OBJECT "/session/%s/%s", munged, st);
                    }
                    if (!strcmp(method, "SelectDevices") && mode >= 8) {
                        const char *restore = get_option(msg, "restore_token");
                        const char *expected = mode == 8 ? "fixture-restore" : mode == 9 ? "paired-first" :
                            mode == 10 ? "paired-next" : mode == 11 ? "paired-next" : NULL;
                        if (expected ? (restore && !strcmp(expected, restore)) : !restore)
                            atomic_fetch_add(&stats->restore_matches, 1);
                    }
                    DBusMessageIter opt;
                    if (!strcmp(method, "SelectSources") &&
                        (find_option(msg, "restore_token", &opt) || find_option(msg, "persist_mode", &opt)))
                        atomic_fetch_add(&stats->bad_calls, 1);
                    if (!strcmp(method, "Start")) atomic_fetch_add(&stats->starts, 1);
                    DBusMessage *reply = dbus_message_new_method_return(msg);
                    const char *path_arg = path;
                    dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &path_arg, DBUS_TYPE_INVALID);
                    /* Deliberately deliver Response before the method reply. */
                    if (!((mode == 2 || mode == 4) && !strcmp(method, "Start"))) {
                        DBusMessage *signal = dbus_message_new_signal(path, REQUEST, "Response");
                        dbus_message_set_destination(signal, sender);
                        DBusMessageIter args, dict; uint32_t code = mode == 1 && !strcmp(method, "Start") ? 1 : 0;
                        dbus_message_iter_init_append(signal, &args);
                        dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &code);
                        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
                        if (!strcmp(method, "CreateSession")) {
                            const char *s = session; dict_value(&dict, "session_handle", DBUS_TYPE_STRING, &s);
                        } else if (!strcmp(method, "Start")) {
                            uint32_t devices = mode == 3 || mode == 11 ? 1 : 3;
                            const char *t = mode == 8 ? "paired-first" : mode >= 9 ? "paired-next" : "fixture-restore";
                            dict_value(&dict, "devices", DBUS_TYPE_UINT32, &devices);
                            dict_value(&dict, "restore_token", DBUS_TYPE_STRING, &t);
                            if (mode >= 8 && mode != 12) {
                                DBusMessageIter entry, variant, array, stream, props;
                                const char *key = "streams", *mapping = "fixture-monitor";
                                uint32_t node = 42;
                                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
                                dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
                                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(ua{sv})", &variant);
                                dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "(ua{sv})", &array);
                                dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &stream);
                                dbus_message_iter_append_basic(&stream, DBUS_TYPE_UINT32, &node);
                                dbus_message_iter_open_container(&stream, DBUS_TYPE_ARRAY, "{sv}", &props);
                                dict_value(&props, "mapping_id", DBUS_TYPE_STRING, &mapping);
                                dbus_message_iter_close_container(&stream, &props);
                                dbus_message_iter_close_container(&array, &stream);
                                dbus_message_iter_close_container(&variant, &array);
                                dbus_message_iter_close_container(&entry, &variant);
                                dbus_message_iter_close_container(&dict, &entry);
                            }
                        }
                        dbus_message_iter_close_container(&args, &dict); send_message(bus, signal);
                    }
                    send_message(bus, reply);
                    if (mode == 4 && !strcmp(method, "Start")) {
                        dbus_bus_release_name(bus, PORTAL, NULL);
                        replacement_owner = dbus_bus_get_private(DBUS_BUS_SESSION, NULL);
                        if (!replacement_owner || dbus_bus_request_name(replacement_owner, PORTAL,
                            DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) _exit(2);
                        /* The old unique owner stays alive and still handles Close. */
                    }
                }
            } else if (dbus_message_is_method_call(msg, SESSION, "Close") || dbus_message_is_method_call(msg, REQUEST, "Close")) {
                atomic_fetch_add(&stats->closes, 1);
                if (dbus_message_has_interface(msg, SESSION)) {
                    if (!dbus_message_has_path(msg, session)) atomic_fetch_add(&stats->bad_calls, 1);
                    atomic_fetch_add(&stats->session_closes, 1);
                }
                send_message(bus, dbus_message_new_method_return(msg));
                ack = 'C'; if (write(notices, &ack, 1) != 1) _exit(2);
            }
            dbus_message_unref(msg);
        }
        eis_dispatch(eis);
        struct eis_event *event;
        while ((event = eis_get_event(eis))) {
            switch (eis_event_get_type(event)) {
            case EIS_EVENT_CLIENT_CONNECT: {
                struct eis_client *client = eis_event_get_client(event); eis_client_connect(client);
                struct eis_seat *seat = eis_client_new_seat(client, "fixture");
                owned_seat = seat;
                eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
                eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
                eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
                eis_seat_configure_capability(seat, EIS_DEVICE_CAP_SCROLL);
                eis_seat_add(seat); break;
            }
            case EIS_EVENT_SEAT_BIND:
                /* Unbind (including on sender disconnect) must not construct devices. */
                if (!eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD) || keyboard) break;
                keyboard = add_keyboard(eis_event_get_seat(event), mode);
                if (mode >= 8) decoy = add_pointer(eis_event_get_seat(event), mode, true);
                pointer = add_pointer(eis_event_get_seat(event), mode, false); break;
            case EIS_EVENT_CLIENT_DISCONNECT:
                if (keyboard) { eis_device_remove(keyboard); keyboard = eis_device_unref(keyboard); }
                if (pointer) { eis_device_remove(pointer); pointer = eis_device_unref(pointer); }
                if (decoy) { eis_device_remove(decoy); decoy = eis_device_unref(decoy); }
                if (owned_seat) { eis_seat_remove(owned_seat); owned_seat = eis_seat_unref(owned_seat); }
                break;
            case EIS_EVENT_KEYBOARD_KEY:
                if (eis_event_keyboard_get_key(event) == 30) atomic_fetch_add(&stats->keys, 1);
                if (eis_event_keyboard_get_key(event) == 42) atomic_fetch_add(&stats->shifts, 1);
                if (eis_event_keyboard_get_key(event) == 50) atomic_fetch_add(&stats->ms, 1);
                if (eis_event_keyboard_get_key(event) == 2) atomic_fetch_add(&stats->ones, 1);
                ack = 'E'; if (write(notices, &ack, 1) != 1) _exit(2); break;
            case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
                if (eis_event_pointer_get_absolute_x(event) == (mode >= 8 ? 1250 : 100) &&
                    eis_event_pointer_get_absolute_y(event) == (mode >= 8 ? 500 : 200) &&
                    eis_event_get_device(event) == pointer)
                    atomic_fetch_add(&stats->moves, 1);
                ack = 'E'; if (write(notices, &ack, 1) != 1) _exit(2); break;
            case EIS_EVENT_BUTTON_BUTTON:
                if (eis_event_button_get_button(event) == 272) atomic_fetch_add(&stats->buttons, 1);
                ack = 'E'; if (write(notices, &ack, 1) != 1) _exit(2); break;
            case EIS_EVENT_SCROLL_DISCRETE:
                if (eis_event_scroll_get_discrete_dx(event) == 120 && eis_event_scroll_get_discrete_dy(event) == -240)
                    atomic_fetch_add(&stats->scrolls, 1);
                ack = 'E'; if (write(notices, &ack, 1) != 1) _exit(2); break;
            default: break;
            }
            eis_event_unref(event);
        }
        struct pollfd fds[] = {{busfd,POLLIN,0}, {eis_get_fd(eis),POLLIN,0}, {commands,POLLIN,0}};
        if (poll(fds, 3, 5000) <= 0) break;
        if (fds[2].revents) {
            char command;
            if (read(commands, &command, 1) != 1 || command == 'Q') break;
            if (command == 'P' && pointer) eis_device_pause(pointer);
            if (command == 'C') {
                send_message(bus, dbus_message_new_signal(session, SESSION, "Closed"));
                /* Barrier on the same sender connection: the daemon routes
                 * Closed to subscribers before replying to this GetId. */
                char *id = dbus_bus_get_id(bus, NULL); dbus_free(id);
            }
            if (command == 'P' || command == 'C') { ack = command; if (write(notices,&ack,1) != 1) _exit(2); }
        }
    }
    if (pointer) { eis_device_remove(pointer); eis_device_unref(pointer); }
    if (decoy) { eis_device_remove(decoy); eis_device_unref(decoy); }
    if (keyboard) { eis_device_remove(keyboard); eis_device_unref(keyboard); }
    if (owned_seat) { eis_seat_remove(owned_seat); eis_seat_unref(owned_seat); }
    if (replacement_owner) { dbus_connection_close(replacement_owner); dbus_connection_unref(replacement_owner); }
    for (int i = 0; i < remote_count; i++) close(remotes[i]);
    eis_unref(eis); dbus_connection_close(bus); dbus_connection_unref(bus);
    _exit(0);
}

static bool notice(int fd, char expected) {
    int64_t deadline = md_now_ms() + 2000;
    while (md_now_ms() < deadline) {
        struct pollfd p = {fd,POLLIN,0};
        int n = poll(&p,1,(int)(deadline-md_now_ms()));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        char c; if (read(fd,&c,1) != 1) return false;
        if (c == expected) return true;
    }
    return false;
}

typedef struct {
    MdCaptureCtx *capture;
    int status;
} CaptureWait;

static void *wait_for_frame(void *data) {
    CaptureWait *wait = data;
    MdFrame frame;
    wait->status = md_capture_get_frame(wait->capture, &frame, 4000);
    if (wait->status == 0) md_capture_release_frame(wait->capture, &frame);
    return NULL;
}

static void check_paired(int mode, int commands, int notices) {
    MdCaptureConfig cfg = {.portal_input = true, .consent_timeout_ms = 1000, .no_interaction = true};
    MdCaptureCtx *cap = md_capture_create(&cfg);
    MD_CHECK(cap != NULL);
    if (!cap) return;
    MD_CHECK(md_capture_start(cap) == MD_CAPTURE_INTERACTION_REQUIRED);
    MD_CHECK(atomic_load(&stats->requests) == 0);
    md_capture_destroy(cap);

    cfg.no_interaction = false;
    cap = md_capture_create(&cfg);
    MD_CHECK(cap != NULL);
    if (!cap) return;
    int started = md_capture_start(cap);
    MD_CHECK(started == (mode < 11 ? 0 : -1));
    MD_CHECK(atomic_load(&stats->requests) == 4);
    MD_CHECK(atomic_load(&stats->starts) == 1);
    MD_CHECK(atomic_load(&stats->restore_matches) == 1);
    MD_CHECK(atomic_load(&stats->opens) == (mode == 11 || mode == 12 ? 0 : 1));
    MD_CHECK(atomic_load(&stats->eis_connections) == (mode == 11 || mode == 12 ? 0 : 1));
    if (started == 0 && mode < 11) {
        MdInputConfig input_cfg = {.screen_width = 1920, .screen_height = 1080,
                                   .portal_session = md_capture_portal_session(cap)};
        MD_CHECK(input_cfg.portal_session != NULL);
        MdInput *input = input_cfg.portal_session ? md_input_create(&input_cfg) : NULL;
        MD_CHECK(input != NULL);
        if (input) {
            MD_CHECK(md_input_is_ready(input));
            MD_CHECK(md_input_mouse_move(input, 100, 200) == 0);
            MD_CHECK(notice(notices, 'E'));
            MD_CHECK(atomic_load(&stats->moves) == 1);
            if (mode == 8) {
                md_capture_stop(cap);
                MD_CHECK(md_capture_start(cap) == 0);
                MD_CHECK(atomic_load(&stats->opens) == 2);
                MD_CHECK(md_input_is_ready(input));
                md_capture_destroy(cap);
                cap = NULL;
                MD_CHECK(atomic_load(&stats->session_closes) == 0);
                MD_CHECK(md_input_mouse_move(input, 100, 200) == 0);
                MD_CHECK(notice(notices, 'E'));
                MD_CHECK(atomic_load(&stats->moves) == 2);
            } else if (mode == 9) {
                MD_CHECK(input->vtable->key_event(input, 30, 1) == 0);
                MD_CHECK(notice(notices, 'E'));
                md_input_destroy(input);
                input = NULL;
                MD_CHECK(notice(notices, 'E')); /* retained EIS released the held key */
                MD_CHECK(atomic_load(&stats->keys) == 2);
                MD_CHECK(atomic_load(&stats->session_closes) == 0);
                md_capture_stop(cap);
                MD_CHECK(md_capture_start(cap) == 0);
                MD_CHECK(atomic_load(&stats->opens) == 2);
            } else {
                CaptureWait wait = {.capture = cap};
                pthread_t thread;
                int rc = pthread_create(&thread, NULL, wait_for_frame, &wait);
                MD_CHECK(rc == 0);
                int64_t now = md_now_ms();
                MD_CHECK(write(commands, "C", 1) == 1);
                MD_CHECK(notice(notices, 'C'));
                if (!rc) {
                    MD_CHECK(pthread_join(thread, NULL) == 0);
                    MD_CHECK(wait.status == MD_CAPTURE_ERROR);
                    MD_CHECK(md_now_ms() - now < 4000);
                }
                MD_CHECK(!md_input_is_ready(input));
                MD_CHECK(md_input_mouse_move(input, 100, 200) == -1);
                md_capture_stop(cap);
                MD_CHECK(md_capture_start(cap) == -1);
                MD_CHECK(atomic_load(&stats->opens) == 1);
            }
            md_input_destroy(input);
        }
    }
    md_capture_destroy(cap);
    MD_CHECK(atomic_load(&stats->bad_calls) == 0);
    MD_CHECK(atomic_load(&stats->starts) == 1);
    MD_CHECK(atomic_load(&stats->session_closes) == 1);
}

int main(void) {
    /* Must be launched on a private bus by the wrapper below. */
    if (!getenv("MD_PORTAL_PRIVATE_TEST")) return 77;
    signal(SIGPIPE, SIG_IGN);
    stats = mmap(NULL,sizeof(*stats),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    MD_CHECK(stats != MAP_FAILED);
    if (stats == MAP_FAILED) return md_test_report();
    MD_CHECK(md_capture_restore_token_save("independent-screencast") == 0);
    for (int mode=0; mode<15; mode++) {
        memset(stats,0,sizeof(*stats));
        int commands[2], notices[2];
        if (pipe(commands) || pipe(notices)) return 1;
        pid_t child = fork();
        if (!child) { close(commands[1]); close(notices[0]); server(commands[0],notices[1],mode); }
        close(commands[0]); close(notices[1]);
        MD_CHECK(child > 0); MD_CHECK(notice(notices[0],'R'));
        MD_CHECK(md_input_portal_available());
        if (mode >= 8) {
            check_paired(mode, commands[1], notices[0]);
            goto finish;
        }
        MdInputConfig cfg = { .screen_width=1920,.screen_height=1080,.backend="portal",.portal_timeout_ms=2000 };
        /* Later modes already have a saved token from mode 0. Even with a
         * token, managed mode must not gamble on Start being prompt-free. */
        cfg.no_interaction = true;
        MD_CHECK(md_input_create(&cfg) == NULL);
        MD_CHECK(atomic_load(&stats->requests) == 0);
        cfg.no_interaction = false;
        uint64_t start = md_monotonic_ns();
        MdInput *input = md_input_create(&cfg);
        if (!mode || mode >= 5) {
            MD_CHECK(input != NULL);
            if (input) {
                MD_CHECK(md_input_is_ready(input));
                MD_CHECK(input->vtable->key_event(input,30,1) == 0);
                MD_CHECK(input->vtable->key_event(input,30,0) == 0);
                /* Equal-sized logical region avoids rounding in this check. */
                MD_CHECK(md_input_mouse_move(input,100,200) == 0);
                MD_CHECK(input->vtable->mouse_button(input,0,1) == 0);
                MD_CHECK(input->vtable->mouse_button(input,0,0) == 0);
                MD_CHECK(md_input_scroll(input,1,2) == 0);
                for (int i=0;i<6;i++) MD_CHECK(notice(notices[0],'E'));
                MD_CHECK(atomic_load(&stats->keys) == 2);
                MD_CHECK(atomic_load(&stats->moves) == 1);
                MD_CHECK(atomic_load(&stats->buttons) == 2);
                MD_CHECK(atomic_load(&stats->scrolls) == 1);
                MD_CHECK(md_input_type_text(input,"a\xc3\xa9") == -1);
                MD_CHECK(atomic_load(&stats->keys) == 2); /* preflight: no partial 'a' */
                if (mode >= 6) {
                    MD_CHECK(md_input_type_text(input,"aA") == -1);
                    MD_CHECK(atomic_load(&stats->keys) == 2);
                    MD_CHECK(atomic_load(&stats->shifts) == 0);
                } else {
                    MD_CHECK(md_input_type_text(input,"Ma!") == 0);
                    for (int i=0;i<10;i++) MD_CHECK(notice(notices[0],'E'));
                    MD_CHECK(atomic_load(&stats->keys) == 4);
                    MD_CHECK(atomic_load(&stats->ms) == 2);
                    MD_CHECK(atomic_load(&stats->ones) == 2);
                    MD_CHECK(atomic_load(&stats->shifts) == 4);
                }
                char command='C'; MD_CHECK(write(commands[1],&command,1) == 1);
                MD_CHECK(notice(notices[0],'C'));
                /* is_ready synchronously drains revocation, not a timer. */
                MD_CHECK(!md_input_is_ready(input));
                MD_CHECK(input->vtable->key_event(input,30,1) == -1);
                md_input_destroy(input);
            }
        } else { MD_CHECK(input == NULL); md_input_destroy(input); }
        MD_CHECK(md_monotonic_ns()-start < 5000000000ull);
        MD_CHECK(notice(notices[0],'C')); MD_CHECK(atomic_load(&stats->closes) >= 1);
finish: ;
        char quit='Q'; MD_CHECK(write(commands[1],&quit,1) == 1);
        close(commands[1]); close(notices[0]); int status;
        MD_CHECK(waitpid(child,&status,0) == child); MD_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    char screen_token[64];
    MD_CHECK(md_capture_restore_token_load(screen_token, sizeof(screen_token)) == strlen("independent-screencast"));
    MD_CHECK(!strcmp(screen_token, "independent-screencast"));
    munmap(stats,sizeof(*stats));
    return md_test_report();
}
