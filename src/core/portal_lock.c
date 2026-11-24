#include "portal_lock.h"
#include "capture.h"
#include "log.h"
#include "md_time.h"

#include <stdlib.h>
#include <string.h>

#define MD_LOG_TAG "portal_lock"
#define SCREENSAVER "org.gnome.ScreenSaver"
#define SCREENSAVER_PATH "/org/gnome/ScreenSaver"

static int remaining_ms(uint64_t deadline, int cap) {
    uint64_t now = md_monotonic_ns();
    if (now >= deadline) return 0;
    uint64_t ms = (deadline - now + 999999) / 1000000;
    return ms < (uint64_t)cap ? (int)ms : cap;
}

static DBusMessage *query(DBusConnection *bus, DBusMessage *message,
                          uint64_t deadline, DBusError *error) {
    if (!message) return NULL;
    int timeout = remaining_ms(deadline, 1000);
    dbus_message_set_auto_start(message, FALSE);
    DBusMessage *reply = timeout ? dbus_connection_send_with_reply_and_block(
        bus, message, timeout, error) : NULL;
    dbus_message_unref(message);
    return reply;
}

static bool gnome_desktop(void) {
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    while (desktop && *desktop) {
        size_t len = strcspn(desktop, ":");
        if (len == 5 && !strncmp(desktop, "GNOME", len)) return true;
        desktop = desktop[len] ? desktop + len + 1 : NULL;
    }
    return false;
}

static int get_active(DBusConnection *bus, uint64_t deadline, char **owner,
                      bool *gnome_seen) {
    DBusError error = DBUS_ERROR_INIT;
    DBusMessage *reply = query(bus, dbus_message_new_method_call(
        SCREENSAVER, SCREENSAVER_PATH, SCREENSAVER, "GetActive"), deadline, &error);
    int status = MD_CAPTURE_ERROR;
    dbus_bool_t active;
    if (reply) {
        const char *sender = dbus_message_get_sender(reply);
        if (sender && sender[0] == ':' && dbus_message_has_signature(reply, "b") &&
            dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID)) {
            *gnome_seen = true;
            status = active ? MD_CAPTURE_DESKTOP_LOCKED : 0;
            if (active && owner && !(*owner = strdup(sender))) status = MD_CAPTURE_ERROR;
        }
        dbus_message_unref(reply);
    } else if (!*gnome_seen && !gnome_desktop() &&
               (dbus_error_has_name(&error, DBUS_ERROR_NAME_HAS_NO_OWNER) ||
                dbus_error_has_name(&error, DBUS_ERROR_SERVICE_UNKNOWN))) {
        /* Other compositors need not implement GNOME's screensaver API. */
        status = 0;
    }
    dbus_error_free(&error);
    if (status == MD_CAPTURE_ERROR) MD_LOG_W("cannot determine GNOME desktop lock state");
    return status;
}

static bool change_match(DBusConnection *bus, uint64_t deadline, const char *method) {
    DBusMessage *msg = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, method);
    const char *rule = "type='signal',sender='" SCREENSAVER "',path='" SCREENSAVER_PATH
                       "',interface='" SCREENSAVER "',member='ActiveChanged'";
    if (msg && !dbus_message_append_args(msg, DBUS_TYPE_STRING, &rule, DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return false;
    }
    DBusMessage *reply = query(bus, msg, deadline, NULL);
    if (!reply) return false;
    dbus_message_unref(reply);
    return true;
}

int md_portal_check_lock(DBusConnection *bus, uint64_t deadline, bool wait_for_unlock,
                         bool *gnome_seen) {
    if (!wait_for_unlock) return get_active(bus, deadline, NULL, gnome_seen);

    /* Install the match before sampling to avoid missing a concurrent unlock. */
    if (!change_match(bus, deadline, "AddMatch")) return MD_CAPTURE_ERROR;
    char *owner = NULL;
    int status = get_active(bus, deadline, &owner, gnome_seen);
    if (status == MD_CAPTURE_DESKTOP_LOCKED) {
        MD_LOG_I("desktop locked; waiting for local unlock before starting capture");
        int remaining;
        while (status == MD_CAPTURE_DESKTOP_LOCKED &&
               (remaining = remaining_ms(deadline, 180000)) > 0) {
            DBusMessage *signal = dbus_connection_pop_message(bus);
            if (!signal) {
                if (!dbus_connection_read_write(bus, remaining)) break;
                continue;
            }
            dbus_bool_t active;
            bool unlocked = dbus_message_is_signal(signal, SCREENSAVER, "ActiveChanged") &&
                dbus_message_has_path(signal, SCREENSAVER_PATH) &&
                dbus_message_has_sender(signal, owner) && dbus_message_has_signature(signal, "b") &&
                dbus_message_get_args(signal, NULL, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID) && !active;
            dbus_message_unref(signal);
            /* An old queued signal is not proof of current state. Re-read
             * only on a candidate unlock, never on a timer. */
            if (unlocked && remaining_ms(deadline, 1))
                status = get_active(bus, deadline, NULL, gnome_seen);
        }
    }
    free(owner);
    /* On error the caller closes this private connection. On success, stop
     * receiving screensaver traffic before starting portal negotiation. */
    if (status == 0 && !change_match(bus, deadline, "RemoveMatch")) return MD_CAPTURE_ERROR;
    return status;
}
