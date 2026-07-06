/*
 * metadesk — ipc_unix.c
 * Unix domain socket IPC backend (Linux / macOS).
 *
 * Endpoints are created as filesystem sockets under XDG_RUNTIME_DIR.
 * The socket path is:
 *   $XDG_RUNTIME_DIR/metadesk-<name>.sock
 *
 * XDG_RUNTIME_DIR must be a private, user-owned directory. Only stale
 * socket files are unlinked before binding; other existing path types fail.
 */
#include "ipc.h"

#ifndef _WIN32  /* entire file is POSIX-only */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Path construction ───────────────────────────────────────── */

#define MD_IPC_PATH_MAX 256

static bool ipc_runtime_dir_is_private(const char *runtime_dir) {
    if (!runtime_dir || runtime_dir[0] == '\0') return false;

    struct stat st;
    if (stat(runtime_dir, &st) < 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    if (st.st_uid != getuid()) return false;
    if ((st.st_mode & 0077) != 0) return false;

    return true;
}

static bool ipc_build_path(const char *name, char *path, size_t path_len) {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!ipc_runtime_dir_is_private(runtime_dir)) return false;

    int ret = snprintf(path, path_len, "%s/metadesk-%s.sock", runtime_dir, name);
    return ret >= 0 && (size_t)ret < path_len;
}

static int ipc_fill_addr(const char *path, struct sockaddr_un *addr) {
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, path_len + 1);
    return 0;
}

static int ipc_unlink_stale_socket(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (!S_ISSOCK(st.st_mode)) {
        errno = EEXIST;
        return -1;
    }

    return unlink(path);
}

/* ── Structures ──────────────────────────────────────────────── */

struct MdIpcServer {
    int  fd;
    char path[MD_IPC_PATH_MAX];
};

struct MdIpcConn {
    int  fd;
    bool connected;
};

/* ── Write/read helpers ──────────────────────────────────────── */

/* Write all bytes, handling partial writes and EINTR. */
static int write_all(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* ── Server API ──────────────────────────────────────────────── */

MdIpcServer *md_ipc_listen(const char *name) {
    if (!md_ipc_name_is_valid(name)) return NULL;

    MdIpcServer *srv = calloc(1, sizeof(MdIpcServer));
    if (!srv) return NULL;
    srv->fd = -1;

    if (!ipc_build_path(name, srv->path, sizeof(srv->path))) {
        free(srv);
        return NULL;
    }

    if (ipc_unlink_stale_socket(srv->path) < 0) {
        fprintf(stderr, "ipc: refusing to replace non-socket endpoint '%s': %s\n",
                srv->path, strerror(errno));
        free(srv);
        return NULL;
    }

    srv->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv->fd < 0) {
        free(srv);
        return NULL;
    }

    struct sockaddr_un addr;
    if (ipc_fill_addr(srv->path, &addr) < 0) {
        fprintf(stderr, "ipc: socket path too long for '%s'\n", srv->path);
        close(srv->fd);
        free(srv);
        return NULL;
    }

    mode_t old_umask = umask(077);
    int bind_ret = bind(srv->fd, (struct sockaddr *)&addr, sizeof(addr));
    int bind_errno = errno;
    umask(old_umask);

    if (bind_ret < 0) {
        errno = bind_errno;
        fprintf(stderr, "ipc: bind failed for '%s': %s\n", srv->path, strerror(errno));
        close(srv->fd);
        free(srv);
        return NULL;
    }

    /* Ensure owner-only permissions even on platforms with unusual defaults. */
    if (chmod(srv->path, 0600) < 0) {
        fprintf(stderr, "ipc: chmod failed for '%s': %s\n", srv->path, strerror(errno));
        close(srv->fd);
        unlink(srv->path);
        free(srv);
        return NULL;
    }

    if (listen(srv->fd, 4) < 0) {
        close(srv->fd);
        unlink(srv->path);
        free(srv);
        return NULL;
    }

    return srv;
}

MdIpcConn *md_ipc_accept(MdIpcServer *srv, uint32_t timeout_ms) {
    if (!srv) return NULL;

    /* Wait for incoming connection with optional timeout */
    if (timeout_ms > 0) {
        struct pollfd pfd = { .fd = srv->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, (int)timeout_ms);
        if (ret <= 0) return NULL; /* timeout or error */
    }

    int client_fd = accept(srv->fd, NULL, NULL);
    if (client_fd < 0) return NULL;

    MdIpcConn *conn = calloc(1, sizeof(MdIpcConn));
    if (!conn) {
        close(client_fd);
        return NULL;
    }

    conn->fd = client_fd;
    conn->connected = true;
    return conn;
}

const char *md_ipc_server_path(const MdIpcServer *srv) {
    return srv ? srv->path : NULL;
}

void md_ipc_server_destroy(MdIpcServer *srv) {
    if (!srv) return;
    close(srv->fd);
    unlink(srv->path);
    free(srv);
}

/* ── Client API ──────────────────────────────────────────────── */

MdIpcConn *md_ipc_connect(const char *name, uint32_t timeout_ms) {
    if (!md_ipc_name_is_valid(name)) return NULL;

    char path[MD_IPC_PATH_MAX];
    if (!ipc_build_path(name, path, sizeof(path))) return NULL;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_un addr;
    if (ipc_fill_addr(path, &addr) < 0) {
        close(fd);
        return NULL;
    }

    /* Non-blocking connect with timeout */
    if (timeout_ms > 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (ret < 0 && errno != EINPROGRESS) {
            close(fd);
            return NULL;
        }

        if (ret < 0) {
            /* Wait for connect to complete */
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            ret = poll(&pfd, 1, (int)timeout_ms);
            if (ret <= 0) {
                close(fd);
                return NULL;
            }

            /* Check for connect error */
            int err = 0;
            socklen_t err_len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len);
            if (err != 0) {
                close(fd);
                return NULL;
            }
        }

        /* Restore blocking mode */
        fcntl(fd, F_SETFL, flags);
    } else {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return NULL;
        }
    }

    MdIpcConn *conn = calloc(1, sizeof(MdIpcConn));
    if (!conn) {
        close(fd);
        return NULL;
    }

    conn->fd = fd;
    conn->connected = true;
    return conn;
}

/* ── Connection I/O ──────────────────────────────────────────── */

int md_ipc_send(MdIpcConn *conn, const void *data, size_t len) {
    if (!conn || !conn->connected || !data) return -1;
    int ret = write_all(conn->fd, data, len);
    if (ret < 0) conn->connected = false;
    return ret;
}

int md_ipc_recv(MdIpcConn *conn, void *buf, size_t buf_len,
                uint32_t timeout_ms) {
    if (!conn || !conn->connected || !buf || buf_len == 0) return -1;

    if (timeout_ms > 0) {
        struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, (int)timeout_ms);
        if (ret < 0) { conn->connected = false; return -1; }
        if (ret == 0) return -1; /* timeout */
    }

    ssize_t n = read(conn->fd, buf, buf_len);
    if (n < 0) {
        if (errno == EINTR) return 0;
        conn->connected = false;
        return -1;
    }
    if (n == 0) {
        conn->connected = false; /* peer disconnected */
        return 0;
    }
    return (int)n;
}

bool md_ipc_is_connected(const MdIpcConn *conn) {
    return conn ? conn->connected : false;
}

void md_ipc_close(MdIpcConn *conn) {
    if (!conn) return;
    if (conn->fd >= 0)
        close(conn->fd);
    free(conn);
}

#endif /* !_WIN32 */
