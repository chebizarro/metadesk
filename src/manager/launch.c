#include "launch.h"
#include <errno.h>
#include <glib-unix.h>
#include <sys/socket.h>
#include <unistd.h>

struct MdManagerLaunch {
    int fd;
    guint source;
    gboolean final, active;
    MdManagerLaunchStatus status;
    gpointer data;
};

static gboolean read_status(int fd, GIOCondition condition, gpointer data) {
    MdManagerLaunch *launch = data;
    MdLaunchMessage msg;
    int rc = 1;
    /* Yield to GTK even if the child keeps its status socket readable. */
    for (unsigned i = 0; i < 32 && (rc = md_managed_launch_message_read(fd, &msg, true)) == 1; ++i) {
        if (msg == MD_LAUNCH_ENDED && launch->active) {
            launch->active = FALSE;
            launch->status(msg, launch->data);
            continue;
        }
        if (msg >= MD_LAUNCH_CANCEL) { errno = EPROTO; rc = -1; break; }
        if (launch->final) continue;
        launch->final = msg >= MD_LAUNCH_CONNECTED;
        launch->active = msg == MD_LAUNCH_CONNECTED;
        launch->status(msg, launch->data);
    }
    if (rc == 1 || (rc < 0 && errno == EAGAIN && !(condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))))
        return G_SOURCE_CONTINUE;
    launch->source = 0;
    close(launch->fd);
    launch->fd = -1;
    if (launch->active) {
        launch->active = FALSE;
        launch->status(MD_LAUNCH_ENDED, launch->data);
    }
    if (!launch->final) {
        launch->final = TRUE;
        launch->status(MD_LAUNCH_FAILED, launch->data);
    }
    return G_SOURCE_REMOVE;
}

MdManagerLaunch *md_manager_launch_start(const char *client, const MdManagedLaunch *config,
    MdManagerLaunchStatus status, gpointer data, GError **error) {
    g_return_val_if_fail(client && *client && config && status, NULL);
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) < 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not open a private client channel");
        return NULL;
    }
    if (!md_managed_launch_write(fds[0], config)) {
        close(fds[0]);
        close(fds[1]);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid client launch configuration");
        return NULL;
    }
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    /* Don't enable INHERIT_FDS. The launcher owns the explicit child FD. */
    g_subprocess_launcher_take_fd(launcher, fds[1], 3);
    g_autoptr(GSubprocess) child = g_subprocess_launcher_spawn(launcher, error,
        client, "--managed-session-fd", "3", NULL);
    if (!child) { close(fds[0]); return NULL; }
    MdManagerLaunch *launch = g_new0(MdManagerLaunch, 1);
    launch->fd = fds[0];
    launch->status = status;
    launch->data = data;
    launch->source = g_unix_fd_add(launch->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, read_status, launch);
    return launch;
}

gboolean md_manager_launch_cancel(MdManagerLaunch *launch) {
    return launch && !launch->final && md_managed_launch_message_write(launch->fd, MD_LAUNCH_CANCEL);
}

gboolean md_manager_launch_disconnect(MdManagerLaunch *launch) {
    return launch && launch->active && md_managed_launch_message_write(launch->fd, MD_LAUNCH_DISCONNECT);
}

void md_manager_launch_free(MdManagerLaunch *launch) {
    if (!launch) return;
    if (launch->source) g_source_remove(launch->source);
    if (launch->fd >= 0) close(launch->fd);
    g_free(launch);
}
