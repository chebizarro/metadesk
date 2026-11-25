#pragma once

#include "managed_launch.h"
#include <gio/gio.h>

typedef struct MdManagerLaunch MdManagerLaunch;
typedef void (*MdManagerLaunchStatus)(MdLaunchMessage message, gpointer data);

/* Main-context owned. The callback reports child progress/terminal status. A
 * missing terminal packet is a failure. Do not call free from the callback. */
MdManagerLaunch *md_manager_launch_start(const char *client, const MdManagedLaunch *config,
    MdManagerLaunchStatus status, gpointer data, GError **error);
gboolean md_manager_launch_cancel(MdManagerLaunch *);
gboolean md_manager_launch_disconnect(MdManagerLaunch *);
/* EOF cancels an unfinished launch; a handed-off video window survives. */
void md_manager_launch_free(MdManagerLaunch *);
