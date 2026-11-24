#ifndef MD_PORTAL_LOCK_H
#define MD_PORTAL_LOCK_H

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stdint.h>

/* 0 permits portal negotiation; otherwise an MD_CAPTURE_* error. The optional
 * wait consumes bus messages, so use it only before owning any portal handles
 * or subscribing to their signals, on a private session-bus connection. */
int md_portal_check_lock(DBusConnection *bus, uint64_t deadline_ns, bool wait_for_unlock,
                         bool *gnome_seen);

#endif
