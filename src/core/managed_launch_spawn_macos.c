#include "managed_launch.h"
#include "management_ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

bool md_managed_launch_prepare_stream(int fd) {
    int type, enabled = 1;
    struct sockaddr_storage peer;
    socklen_t n = sizeof(type), pn = sizeof(peer);
    return !getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &n) && type == SOCK_STREAM &&
        !getpeername(fd, (struct sockaddr *)&peer, &pn) && peer.ss_family == AF_UNIX &&
        md_management_peer_is_user(fd, (uint64_t)geteuid()) &&
        !setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) &&
        fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0;
}

int md_managed_launch_spawn(const char *client, int *fd, pid_t *pid) {
    if (!client || *client != '/' || !fd || !pid) return EINVAL;
    *fd = -1;
    *pid = -1;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) return errno;
    int child = -1;
    int rc;
    /* Darwin lacks SOCK_CLOEXEC for socketpair. An unrelated concurrent
     * fork/exec could still inherit an end between creation and fcntl.
     * Our own spawn separately allowlists descriptors with CLOEXEC_DEFAULT. */
    if (fcntl(sockets[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(sockets[1], F_SETFD, FD_CLOEXEC) < 0) { rc = errno; goto close_sockets; }
    rc = EACCES;
    if (!md_managed_launch_prepare_stream(sockets[0]) ||
        !md_managed_launch_prepare_stream(sockets[1])) goto close_sockets;
    /* Darwin's stream send can block on buffer exhaustion despite
     * MSG_DONTWAIT. The parent's config watchdog needs a nonblocking fd.
     * The separate child endpoint remains blocking for its command reader. */
    if (fcntl(sockets[0], F_SETFL, O_NONBLOCK) < 0) { rc = errno; goto close_sockets; }
    /* Keep the source distinct from standard streams and the target fd 3. */
    child = fcntl(sockets[1], F_DUPFD_CLOEXEC, 4);
    if (child < 0) { rc = errno; goto close_sockets; }
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t actions;
    if ((rc = posix_spawnattr_init(&attr))) goto close_sockets;
    if ((rc = posix_spawn_file_actions_init(&actions))) goto destroy_attr;
    /* The GUI ignores TERM and libdispatch workers mask async signals.
     * Present a fresh process signal state before the client sets up sigwait. */
    sigset_t unblocked, defaults;
    sigemptyset(&unblocked);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGHUP);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGCHLD);
    rc = posix_spawnattr_setsigmask(&attr, &unblocked);
    if (!rc) rc = posix_spawnattr_setsigdefault(&attr, &defaults);
    if (!rc) rc = posix_spawnattr_setflags(&attr,
        POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    for (int i = 0; i < 3 && !rc; ++i)
        rc = posix_spawn_file_actions_addopen(&actions, i, "/dev/null", i ? O_WRONLY : O_RDONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, child, 3);
    if (!rc) rc = posix_spawn_file_actions_addclose(&actions, child);

    /* Forward only runtime locations, not inherited signer credentials,
     * loader overrides, or arbitrary variables from the GUI's environment. */
    static const char *const names[] = {"HOME", "PATH", "TMPDIR", "XDG_STATE_HOME",
        "XDG_RUNTIME_DIR", "NOSTR_SIGNER_SOCK"};
    const size_t count = sizeof(names) / sizeof(*names);
    char *env[sizeof(names) / sizeof(*names) + 1] = {0};
    size_t j = 0;
    for (size_t i = 0; i < count && !rc; ++i) {
        const char *value = getenv(names[i]);
        if (value && asprintf(&env[j], "%s=%s", names[i], value) < 0) rc = ENOMEM;
        else if (value) ++j;
    }
    if (!rc) {
        char *argv[] = {(char *)client, "--managed-session-fd", "3", NULL};
        rc = posix_spawn(pid, client, &actions, &attr, argv, env);
    }
    while (j) free(env[--j]);
    posix_spawn_file_actions_destroy(&actions);
destroy_attr:
    posix_spawnattr_destroy(&attr);
close_sockets:
    if (child >= 0) close(child);
    close(sockets[1]);
    if (rc) close(sockets[0]);
    else *fd = sockets[0];
    return rc;
}
