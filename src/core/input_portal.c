/* RemoteDesktop v2 input-only session. No ScreenCast permission is needed for
 * EIS global regions; capture keeps its separate session/restore token. Never
 * fall back to privileged injection after consent denial or session loss. */
#include "input.h"
#include "log.h"
#include "md_time.h"
#include <dbus/dbus.h>
#include <libei.h>
#include <xkbcommon/xkbcommon.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>
#define MD_LOG_TAG "input_portal"
#define PORTAL "org.freedesktop.portal.Desktop"
#define OBJECT "/org/freedesktop/portal/desktop"
#define REMOTE "org.freedesktop.portal.RemoteDesktop"
#define REQUEST "org.freedesktop.portal.Request"
#define SESSION "org.freedesktop.portal.Session"

typedef struct {
    DBusConnection *bus;
    char owner[256], sender[256], session[512];
    unsigned serial;
    int64_t deadline;
    struct ei *ei;
    struct ei_device *keyboard, *pointer, *button, *scroll;
    pthread_mutex_t mutex;
    pthread_t thread;
    int wake[2];
    bool started, dead;
    uint32_t sequence;
    uint32_t depressed, latched, locked, group;
    bool held[KEY_MAX + 1];
} PortalInput;

static int remaining(PortalInput *p) {
    int64_t ms = p->deadline - md_now_ms();
    return ms > 0 ? (int)ms : 0;
}

/* All startup calls, authentication, responses and EIS negotiation share a
 * monotonic budget. A private bus connection never drains the host's bus. */
static DBusMessage *call(PortalInput *p, DBusMessage *msg) {
    DBusMessage *reply = NULL;
    if (msg && remaining(p)) {
        DBusError err = DBUS_ERROR_INIT;
        reply = dbus_connection_send_with_reply_and_block(p->bus, msg, remaining(p), &err);
        if (dbus_error_is_set(&err)) {
            MD_LOG_I("portal: %s", err.name);
            dbus_error_free(&err);
        }
    }
    if (msg) dbus_message_unref(msg);
    return reply;
}

static void option(DBusMessageIter *dict, const char *key, int type, const void *value) {
    DBusMessageIter entry, variant;
    char sig[2] = { (char)type, 0 };
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &variant);
    dbus_message_iter_append_basic(&variant, type, value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

static bool lookup(DBusMessage *msg, const char *key, int type, void *value) {
    DBusMessageIter args, array;
    if (!dbus_message_has_signature(msg, "ua{sv}")) return false;
    dbus_message_iter_init(msg, &args);
    dbus_message_iter_next(&args);
    dbus_message_iter_recurse(&args, &array);
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        const char *name;
        dbus_message_iter_recurse(&array, &entry);
        dbus_message_iter_get_basic(&entry, &name);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &variant);
        if (!strcmp(name, key) && dbus_message_iter_get_arg_type(&variant) == type) {
            dbus_message_iter_get_basic(&variant, value);
            return true;
        }
        dbus_message_iter_next(&array);
    }
    return false;
}

static void close_object(PortalInput *p, const char *path, const char *interface) {
    if (!p->bus || !path[0]) return;
    DBusMessage *msg = dbus_message_new_method_call(p->owner[0] ? p->owner : PORTAL,
                                                   path, interface, "Close");
    if (msg) {
        /* Cleanup gets its own failure-only bound, including after startup's
         * budget expires. Wait for the portal to acknowledge session closure
         * rather than dropping a queued no-reply message with the bus. */
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(p->bus, msg, 1000, NULL);
        if (reply) dbus_message_unref(reply);
        dbus_message_unref(msg);
    }
}

static pthread_once_t dbus_once = PTHREAD_ONCE_INIT;
static void init_dbus_threads(void) { dbus_threads_init_default(); }

static DBusConnection *connect_bus(PortalInput *p) {
    pthread_once(&dbus_once, init_dbus_threads);
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!address || strncmp(address, "unix:", 5) || strchr(address, ';')) return NULL;
    DBusError err = DBUS_ERROR_INIT;
    p->bus = dbus_connection_open_private(address, &err);
    dbus_error_free(&err);
    if (!p->bus) return NULL;
    dbus_connection_set_exit_on_disconnect(p->bus, FALSE);
    while (!dbus_connection_get_is_authenticated(p->bus)) {
        if (!remaining(p) || !dbus_connection_read_write(p->bus, remaining(p))) return NULL;
    }
    DBusMessage *reply = call(p, dbus_message_new_method_call(DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "Hello"));
    const char *name = NULL;
    bool ok = reply && dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &name,
                                            DBUS_TYPE_INVALID);
    if (ok) {
        snprintf(p->sender, sizeof(p->sender), "%s", name + 1);
        for (char *c = p->sender; *c; c++) if (*c == '.') *c = '_';
    }
    if (reply) dbus_message_unref(reply);
    return ok ? p->bus : NULL;
}

static bool add_match(PortalInput *p, const char *rule) {
    DBusMessage *msg = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "AddMatch");
    if (!msg) return false;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &rule, DBUS_TYPE_INVALID);
    DBusMessage *reply = call(p, msg);
    if (!reply) return false;
    dbus_message_unref(reply);
    return true;
}

/* Capability discovery for explicit portal callers/tests, not auto-selection
 * pending md-y8kc live acceptance. Availability is not authorization. */
bool md_input_portal_available(void) {
    PortalInput p = { .deadline = md_now_ms() + 1000 };
    bool found = false;
    if (connect_bus(&p)) {
        DBusMessage *msg = dbus_message_new_method_call(PORTAL, OBJECT,
            DBUS_INTERFACE_PROPERTIES, "Get");
        const char *iface = REMOTE, *property = "version";
        if (msg) dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
            DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID);
        DBusMessage *reply = call(&p, msg);
        if (reply && dbus_message_has_signature(reply, "v")) {
            DBusMessageIter arg, variant;
            dbus_message_iter_init(reply, &arg);
            dbus_message_iter_recurse(&arg, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32) {
                uint32_t version;
                dbus_message_iter_get_basic(&variant, &version);
                found = version >= 2;
            }
        }
        if (reply) dbus_message_unref(reply);
    }
    if (p.bus) { dbus_connection_close(p.bus); dbus_connection_unref(p.bus); }
    return found;
}

/* Separate from screencast.token: RemoteDesktop tokens carry input authority.
 * Follow capture's owned 0700 leaf + nofollow 0600 file + atomic rename. */
static int token_dir(void) {
    char path[PATH_MAX];
    const char *base = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    int n;
    if (base && base[0] == '/') n = snprintf(path, sizeof(path), "%s/metadesk", base);
    else if ((!base || !base[0]) && home && home[0] == '/')
        n = snprintf(path, sizeof(path), "%s/.local/state/metadesk", home);
    else return -1;
    if (n < 0 || n >= (int)sizeof(path)) return -1;
    for (char *c = path + 1; *c; c++) if (*c == '/') {
        *c = 0; int rc = mkdir(path, 0700); *c = '/';
        if (rc && errno != EEXIST) return -1;
    }
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != geteuid() || fchmod(fd, 0700))) {
        close(fd); fd = -1;
    }
    return fd;
}

static void token_load(char token[1024]) {
    int dir = token_dir();
    if (dir < 0) return;
    int fd = openat(dir, "remote-desktop.token", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    close(dir);
    if (fd < 0) return;
    struct stat st;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
        st.st_nlink <= 1 && st.st_size > 0 && st.st_size < 1024 && !fchmod(fd, 0600)) {
        ssize_t n = read(fd, token, (size_t)st.st_size);
        if (n == st.st_size && !memchr(token, 0, (size_t)n)) token[n] = 0;
        else token[0] = 0;
    }
    close(fd);
    if (!dbus_validate_utf8(token, NULL)) token[0] = 0;
}

static void token_save(const char *token) {
    if (!token || !token[0] || strlen(token) >= 1024) return;
    int dir = token_dir();
    if (dir < 0) return;
    char tmp[80];
    static unsigned serial; /* protected by token mutex below */
    snprintf(tmp, sizeof(tmp), ".remote-desktop.%ld.%u", (long)getpid(), serial++);
    int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        size_t len = strlen(token);
        bool ok = !fchmod(fd, 0600) && write(fd, token, len) == (ssize_t)len && !fsync(fd);
        if (close(fd)) ok = false;
        if (ok && !renameat(dir, tmp, dir, "remote-desktop.token")) fsync(dir);
        unlinkat(dir, tmp, 0);
    }
    close(dir);
}
static pthread_mutex_t token_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Precompute request/session paths before sending so early Response and
 * cancellation both have an owned object to close. Pin the unique owner. */
static void lifecycle(PortalInput *p, DBusMessage *msg);

static DBusMessage *request(PortalInput *p, const char *method, const char *restore) {
    char token[40], path[512];
    snprintf(token, sizeof(token), "md_input_%u", ++p->serial);
    snprintf(path, sizeof(path), OBJECT "/request/%s/%s", p->sender, token);
    const char *handle = token;
    DBusMessage *msg = dbus_message_new_method_call(p->owner[0] ? p->owner : PORTAL,
                                                   OBJECT, REMOTE, method);
    if (!msg) return NULL;
    DBusMessageIter args, dict;
    dbus_message_iter_init_append(msg, &args);
    const char *session = p->session, *parent = "";
    bool create = !strcmp(method, "CreateSession"), start = !strcmp(method, "Start");
    if (!create) dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &session);
    if (start) dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    option(&dict, "handle_token", DBUS_TYPE_STRING, &handle);
    if (create) {
        const char *st = "md_input_session";
        option(&dict, "session_handle_token", DBUS_TYPE_STRING, &st);
        snprintf(p->session, sizeof(p->session), OBJECT "/session/%s/%s", p->sender, st);
    } else if (!start) {
        uint32_t types = 3, persist = 2;
        option(&dict, "types", DBUS_TYPE_UINT32, &types);
        option(&dict, "persist_mode", DBUS_TYPE_UINT32, &persist);
        if (restore && restore[0]) option(&dict, "restore_token", DBUS_TYPE_STRING, &restore);
    }
    dbus_message_iter_close_container(&args, &dict);
    DBusMessage *reply = call(p, msg);
    const char *actual, *owner = reply ? dbus_message_get_sender(reply) : NULL;
    bool accepted = reply && owner && owner[0] == ':' &&
        (!p->owner[0] || !strcmp(owner, p->owner)) &&
        dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &actual, DBUS_TYPE_INVALID) &&
        !strcmp(path, actual);
    if (accepted) snprintf(p->owner, sizeof(p->owner), "%s", owner);
    if (reply) dbus_message_unref(reply);
    if (accepted) while (remaining(p)) {
        DBusMessage *event;
        while (remaining(p) && (event = dbus_connection_pop_message(p->bus))) {
            lifecycle(p, event);
            if (p->dead) { dbus_message_unref(event); goto cancelled; }
            if (dbus_message_is_signal(event, REQUEST, "Response") &&
                dbus_message_has_sender(event, p->owner) && dbus_message_has_path(event, path) &&
                dbus_message_has_signature(event, "ua{sv}")) {
                uint32_t response;
                DBusMessageIter it;
                dbus_message_iter_init(event, &it);
                dbus_message_iter_get_basic(&it, &response);
                if (!response) return event;
                dbus_message_unref(event);
                return NULL;
            }
            dbus_message_unref(event);
        }
        if (!dbus_connection_read_write(p->bus, remaining(p))) break;
    }
cancelled:
    close_object(p, path, REQUEST);
    return NULL;
}

static void device_clear(struct ei_device **slot, struct ei_device *device) {
    if (*slot && (!device || *slot == device)) *slot = ei_device_unref(*slot);
}

/* Called under mutex after init; all libei operations share that lock. */
static void dispatch(PortalInput *p) {
    ei_dispatch(p->ei);
    struct ei_event *event;
    while ((event = ei_get_event(p->ei))) {
        struct ei_device *device = ei_event_get_device(event);
        switch (ei_event_get_type(event)) {
        case EI_EVENT_SEAT_ADDED:
            ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD,
                EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, NULL);
            break;
        case EI_EVENT_DEVICE_RESUMED:
            ei_device_start_emulating(device, ++p->sequence);
#define BIND(slot, cap) if (!p->slot && ei_device_has_capability(device, cap)) p->slot = ei_device_ref(device)
            BIND(keyboard, EI_DEVICE_CAP_KEYBOARD);
            BIND(pointer, EI_DEVICE_CAP_POINTER_ABSOLUTE);
            BIND(button, EI_DEVICE_CAP_BUTTON);
            BIND(scroll, EI_DEVICE_CAP_SCROLL);
#undef BIND
            break;
        case EI_EVENT_KEYBOARD_MODIFIERS:
            if (device == p->keyboard) {
                p->depressed = ei_event_keyboard_get_xkb_mods_depressed(event);
                p->latched = ei_event_keyboard_get_xkb_mods_latched(event);
                p->locked = ei_event_keyboard_get_xkb_mods_locked(event);
                p->group = ei_event_keyboard_get_xkb_group(event);
            }
            break;
        case EI_EVENT_DEVICE_PAUSED:
        case EI_EVENT_DEVICE_REMOVED:
            if (device == p->keyboard) {
                memset(p->held, 0, sizeof(p->held));
                p->depressed = p->latched = p->locked = p->group = 0;
            }
            device_clear(&p->keyboard, device); device_clear(&p->pointer, device);
            device_clear(&p->button, device); device_clear(&p->scroll, device);
            break;
        case EI_EVENT_DISCONNECT: p->dead = true; break;
        default: break;
        }
        ei_event_unref(event);
    }
}

static bool ready(PortalInput *p) {
    return !p->dead && p->keyboard && p->pointer && p->button && p->scroll;
}

static void lifecycle(PortalInput *p, DBusMessage *msg) {
    if ((dbus_message_is_signal(msg, SESSION, "Closed") &&
         dbus_message_has_sender(msg, p->owner) && dbus_message_has_path(msg, p->session)) ||
        dbus_message_is_signal(msg, DBUS_INTERFACE_LOCAL, "Disconnected")) p->dead = true;
    if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged") &&
        dbus_message_has_sender(msg, DBUS_SERVICE_DBUS)) {
        const char *name, *old, *new_owner;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old,
            DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) && !strcmp(name, PORTAL) &&
            strcmp(new_owner, p->owner)) p->dead = true;
    }
}

static void bus_dispatch(PortalInput *p) {
    if (!dbus_connection_read_write(p->bus, 0)) p->dead = true;
    DBusMessage *msg;
    while ((msg = dbus_connection_pop_message(p->bus))) {
        lifecycle(p, msg);
        dbus_message_unref(msg);
    }
}

static void *worker(void *data) {
    PortalInput *p = data;
    int busfd = -1;
    dbus_connection_get_unix_fd(p->bus, &busfd);
    struct pollfd fds[] = {{ei_get_fd(p->ei), POLLIN, 0}, {busfd, POLLIN, 0}, {p->wake[0], POLLIN, 0}};
    for (;;) {
        pthread_mutex_lock(&p->mutex);
        dispatch(p); bus_dispatch(p);
        bool dead = p->dead;
        pthread_mutex_unlock(&p->mutex);
        if (dead) break;
        int n = poll(fds, 3, -1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || fds[2].revents) break;
        if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
            pthread_mutex_lock(&p->mutex); p->dead = true; pthread_mutex_unlock(&p->mutex);
            break;
        }
    }
    return NULL;
}

static void portal_destroy(MdInputCtx *ctx) {
    PortalInput *p = ctx->backend_data;
    if (!p) return;
    if (p->started) { char c = 1; (void)write(p->wake[1], &c, 1); pthread_join(p->thread, NULL); }
    device_clear(&p->keyboard, NULL); device_clear(&p->pointer, NULL);
    device_clear(&p->button, NULL); device_clear(&p->scroll, NULL);
    if (p->ei) ei_unref(p->ei);
    close_object(p, p->session, SESSION);
    if (p->bus) { dbus_connection_close(p->bus); dbus_connection_unref(p->bus); }
    if (p->wake[0] >= 0) close(p->wake[0]);
    if (p->wake[1] >= 0) close(p->wake[1]);
    pthread_mutex_destroy(&p->mutex);
    free(p); ctx->backend_data = NULL; ctx->ready = false;
}

static int portal_init(MdInputCtx *ctx) {
    PortalInput *p = calloc(1, sizeof(*p));
    if (!p) return -1;
    p->wake[0] = p->wake[1] = -1;
    pthread_mutex_init(&p->mutex, NULL);
    ctx->backend_data = p;
    uint32_t timeout = ctx->config.portal_timeout_ms ? ctx->config.portal_timeout_ms : 30000;
    if (timeout > 180000) timeout = 180000;
    p->deadline = md_now_ms() + timeout;
    const char *stage = "session-bus setup";
    if (!connect_bus(p) || !add_match(p, "type='signal',sender='" PORTAL "',interface='" REQUEST "'") ||
        !add_match(p, "type='signal',sender='" PORTAL "',interface='" SESSION "'") ||
        !add_match(p, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" PORTAL "'")) goto fail;
    stage = "CreateSession";
    DBusMessage *resp = request(p, "CreateSession", NULL);
    const char *session = NULL;
    bool ok = resp && lookup(resp, "session_handle", DBUS_TYPE_STRING, &session) && !strcmp(session, p->session);
    if (resp) dbus_message_unref(resp);
    if (!ok) goto fail;
    char token[1024] = {0};
    pthread_mutex_lock(&token_mutex); token_load(token); pthread_mutex_unlock(&token_mutex);
    stage = "SelectDevices";
    resp = request(p, "SelectDevices", token);
    if (!resp) goto fail;
    dbus_message_unref(resp);
    MD_LOG_I("Awaiting RemoteDesktop consent (input only, no uinput)");
    stage = "Start consent";
    resp = request(p, "Start", NULL);
    uint32_t devices = 0;
    ok = resp && lookup(resp, "devices", DBUS_TYPE_UINT32, &devices) && (devices & 3) == 3;
    const char *replacement = NULL;
    if (ok && lookup(resp, "restore_token", DBUS_TYPE_STRING, &replacement)) {
        pthread_mutex_lock(&token_mutex); token_save(replacement); pthread_mutex_unlock(&token_mutex);
    }
    if (resp) dbus_message_unref(resp);
    if (!ok) goto fail;
    stage = "ConnectToEIS";
    DBusMessage *msg = dbus_message_new_method_call(p->owner, OBJECT, REMOTE, "ConnectToEIS");
    if (!msg) goto fail;
    DBusMessageIter args, dict;
    dbus_message_iter_init_append(msg, &args);
    session = p->session;
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &session);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&args, &dict);
    resp = call(p, msg);
    int fd = -1;
    if (resp && dbus_message_has_sender(resp, p->owner))
        dbus_message_get_args(resp, NULL, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
    /* libdbus owns its fd; libei takes ownership of a separate CLOEXEC fd. */
    int owned = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 3) : -1;
    if (resp) dbus_message_unref(resp);
    if (owned < 0) goto fail;
    p->ei = ei_new_sender(p);
    if (!p->ei) { close(owned); goto fail; }
    ei_configure_name(p->ei, "metadesk");
    if (ei_setup_backend_fd(p->ei, owned)) goto fail;
    stage = "EIS device negotiation";
    while (remaining(p)) {
        dispatch(p); bus_dispatch(p);
        if (ready(p)) break;
        if (p->dead) goto fail;
        int busfd = -1; dbus_connection_get_unix_fd(p->bus, &busfd);
        struct pollfd fds[] = {{ei_get_fd(p->ei), POLLIN, 0}, {busfd, POLLIN, 0}};
        int n = poll(fds, 2, remaining(p));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL))) goto fail;
    }
    if (!ready(p) || pipe2(p->wake, O_CLOEXEC | O_NONBLOCK)) goto fail;
    if (pthread_create(&p->thread, NULL, worker, p)) goto fail;
    p->started = true; ctx->ready = true;
    return 0;
fail:
    MD_LOG_W("RemoteDesktop %s unavailable or consent not granted (no fallback)", stage);
    portal_destroy(ctx);
    return -1;
}

static bool portal_ready(const MdInputCtx *ctx) {
    PortalInput *p = ctx->backend_data;
    if (!p) return false;
    pthread_mutex_lock(&p->mutex);
    dispatch(p); bus_dispatch(p);
    bool result = ready(p);
    pthread_mutex_unlock(&p->mutex);
    return result;
}

static int portal_key(MdInputCtx *ctx, uint32_t key, int pressed) {
    PortalInput *p = ctx->backend_data;
    if (!p || key > KEY_MAX) return -1;
    pthread_mutex_lock(&p->mutex);
    dispatch(p); bus_dispatch(p);
    int rc = ready(p) ? 0 : -1;
    if (!rc && p->held[key] != !!pressed) {
        ei_device_keyboard_key(p->keyboard, key, !!pressed);
        ei_device_frame(p->keyboard, ei_now(p->ei));
        p->held[key] = !!pressed;
    }
    pthread_mutex_unlock(&p->mutex);
    return rc;
}

static int portal_move(MdInputCtx *ctx, int x, int y) {
    PortalInput *p = ctx->backend_data;
    if (!p) return -1;
    pthread_mutex_lock(&p->mutex);
    dispatch(p); bus_dispatch(p);
    int rc = -1;
    if (ready(p)) {
        double gx, gy;
        if (ctx->config.output_width) md_input_map_to_output(&ctx->config, x, y, &gx, &gy);
        else {
            struct ei_region *region = ei_device_get_region(p->pointer, 0);
            if (!region || ei_device_get_region(p->pointer, 1)) goto done;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if ((uint32_t)x >= ctx->config.screen_width) x = ctx->config.screen_width - 1;
            if ((uint32_t)y >= ctx->config.screen_height) y = ctx->config.screen_height - 1;
            gx = ei_region_get_x(region) + (double)x * (ei_region_get_width(region) - 1) / (ctx->config.screen_width - 1);
            gy = ei_region_get_y(region) + (double)y * (ei_region_get_height(region) - 1) / (ctx->config.screen_height - 1);
        }
        if (ei_device_get_region_at(p->pointer, gx, gy)) {
            ei_device_pointer_motion_absolute(p->pointer, gx, gy);
            ei_device_frame(p->pointer, ei_now(p->ei)); rc = 0;
        }
    }
done:
    pthread_mutex_unlock(&p->mutex);
    return rc;
}

static int portal_button(MdInputCtx *ctx, int button, int pressed) {
    PortalInput *p = ctx->backend_data;
    static const uint32_t buttons[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    if (!p || button < 0 || button > 2) return -1;
    pthread_mutex_lock(&p->mutex);
    dispatch(p); bus_dispatch(p);
    int rc = ready(p) ? 0 : -1;
    if (!rc) { ei_device_button_button(p->button, buttons[button], !!pressed); ei_device_frame(p->button, ei_now(p->ei)); }
    pthread_mutex_unlock(&p->mutex);
    return rc;
}

static int portal_scroll(MdInputCtx *ctx, int dx, int dy) {
    PortalInput *p = ctx->backend_data;
    if (!p || dx > INT32_MAX / 120 || dx < INT32_MIN / 120 || dy > INT32_MAX / 120 || dy < INT32_MIN / 120) return -1;
    pthread_mutex_lock(&p->mutex);
    dispatch(p); bus_dispatch(p);
    int rc = ready(p) ? 0 : -1;
    if (!rc) { ei_device_scroll_discrete(p->scroll, dx * 120, -dy * 120); ei_device_frame(p->scroll, ei_now(p->ei)); }
    pthread_mutex_unlock(&p->mutex);
    return rc;
}

/* Preflight the entire ASCII string against the compositor-provided keymap.
 * Never assume a US desktop, override held modifiers, or partially type an
 * unsupported string. Unicode composition is deliberately not claimed. */
static int portal_text(MdInputCtx *ctx, const char *text) {
    PortalInput *p = ctx->backend_data;
    if (!p || !text || strnlen(text, 4097) > 4096) return -1;
    pthread_mutex_lock(&p->mutex);
    dispatch(p); bus_dispatch(p);
    int rc = -1;
    struct xkb_context *xctx = NULL;
    struct xkb_keymap *map = NULL;
    struct xkb_state *state = NULL;
    if (!ready(p) || p->depressed || p->latched || p->locked) goto done;
    for (unsigned k = 0; k <= KEY_MAX; k++) if (p->held[k]) goto done;
    struct ei_keymap *keymap = ei_device_keyboard_get_keymap(p->keyboard);
    if (!keymap || ei_keymap_get_type(keymap) != EI_KEYMAP_TYPE_XKB) goto done;
    size_t size = ei_keymap_get_size(keymap);
    struct stat st;
    int fd = ei_keymap_get_fd(keymap);
    if (!size || size > 1024 * 1024 || fstat(fd, &st) || st.st_size < (off_t)size) goto done;
    const char *buffer = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buffer == MAP_FAILED) goto done;
    xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (xctx && buffer[size - 1] == 0)
        map = xkb_keymap_new_from_string(xctx, buffer, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap((void *)buffer, size);
    if (!map || !(state = xkb_state_new(map))) goto done;
    for (const char *c = text; *c; c++) {
        uint32_t key; bool shift;
        if (!md_input_ascii_key(*c, &key, &shift)) goto done;
        xkb_state_update_mask(state, 0, 0, 0, 0, 0, p->group);
        if (shift) xkb_state_update_key(state, KEY_LEFTSHIFT + 8, XKB_KEY_DOWN);
        uint32_t expected = *c == '\n' ? '\r' : (unsigned char)*c;
        if (xkb_state_key_get_utf32(state, key + 8) != expected) goto done;
    }
    for (const char *c = text; *c; c++) {
        uint32_t key; bool shift;
        md_input_ascii_key(*c, &key, &shift);
        if (shift) ei_device_keyboard_key(p->keyboard, KEY_LEFTSHIFT, true);
        ei_device_keyboard_key(p->keyboard, key, true);
        ei_device_frame(p->keyboard, ei_now(p->ei));
        ei_device_keyboard_key(p->keyboard, key, false);
        if (shift) ei_device_keyboard_key(p->keyboard, KEY_LEFTSHIFT, false);
        ei_device_frame(p->keyboard, ei_now(p->ei));
    }
    rc = 0;
done:
    if (state) xkb_state_unref(state);
    if (map) xkb_keymap_unref(map);
    if (xctx) xkb_context_unref(xctx);
    pthread_mutex_unlock(&p->mutex);
    return rc;
}

const MdInputBackend *md_input_portal_backend(void) {
    static const MdInputBackend backend = {
        .init = portal_init, .mouse_move = portal_move, .mouse_button = portal_button,
        .mouse_scroll = portal_scroll, .key_event = portal_key, .type_text = portal_text,
        .destroy = portal_destroy, .is_ready = portal_ready,
    };
    return &backend;
}
