#include "io_util.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

ssize_t md_socket_write(int fd, const void *buf, size_t len)
{
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return send(fd, buf, len, flags);
}

int md_fd_write_deadline(int fd, const void *buf, size_t len,
                         int64_t deadline_ms, bool is_socket, size_t *committed)
{
    const char *p = buf;
    size_t chunk = len;
    if (!is_socket) {
        struct stat st;
        if (fstat(fd, &st) < 0) return -1;
        chunk = S_ISFIFO(st.st_mode) ? PIPE_BUF : 1;
    }
    while (len > 0) {
        int64_t remaining = deadline_ms - md_now_ms();
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int rc = poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (rc < 0 && errno == EINTR) continue;
        if (rc == 0) { errno = ETIMEDOUT; return -1; }
        if (rc < 0) return -1;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EPIPE;
            return -1;
        }
        size_t count = len < chunk ? len : chunk;
        ssize_t n = is_socket ? md_socket_write(fd, p, count) : write(fd, p, count);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0) return -1;
        if (committed) *committed += (size_t)n;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}
