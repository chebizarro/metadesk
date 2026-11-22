#include "io_util.h"
#include "md_socket.h"
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
/* The request and caller's buffer remain owned until the worker has exited,
 * including when cancellation races completion or precedes syscall entry. */
typedef struct {
    HANDLE handle;
    void *buffer;
    DWORD length, transferred, error;
    bool writing;
    volatile LONG cancelled;
} WinFdIo;
static void *win_fd_worker(void *userdata) {
    WinFdIo *op = userdata;
    if (InterlockedCompareExchange(&op->cancelled, 0, 0)) {
        op->error = ERROR_OPERATION_ABORTED;
        return 0;
    }
    BOOL ok = op->writing
        ? WriteFile(op->handle, op->buffer, op->length, &op->transferred, NULL)
        : ReadFile(op->handle, op->buffer, op->length, &op->transferred, NULL);
    op->error = ok ? ERROR_SUCCESS : GetLastError();
    return 0;
}
static ssize_t win_fd_io(int fd, void *buf, size_t len, int timeout_ms, bool writing) {
    intptr_t raw = _get_osfhandle(fd);
    if (raw == -1) { errno = EBADF; return -1; }
    WinFdIo op = { .handle = (HANDLE)raw, .buffer = buf, .writing = writing,
        .length = len > MAXDWORD ? MAXDWORD : (DWORD)len };
    /* Join through the same pthread runtime as the transport. Its native
     * handle is borrowed for cancellation, never closed independently. */
    pthread_t worker;
    int created = pthread_create(&worker, NULL, win_fd_worker, &op);
    if (created) { errno = created; return -1; }
    HANDLE thread = (HANDLE)pthread_gethandle(worker);
    DWORD result = WaitForSingleObject(thread, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    if (result != WAIT_OBJECT_0) {
        InterlockedExchange(&op.cancelled, 1);
        do {
            /* CancelSynchronousIo does not join, and ERROR_NOT_FOUND can mean
             * the worker has not entered ReadFile/WriteFile yet. */
            CancelSynchronousIo(thread);
            result = WaitForSingleObject(thread, 10);
        } while (result == WAIT_TIMEOUT);
    }
    pthread_join(worker, NULL);
    if (op.transferred) return (ssize_t)op.transferred;
    if (op.error == ERROR_SUCCESS || (!writing && op.error == ERROR_BROKEN_PIPE)) return 0;
    errno = op.error == ERROR_OPERATION_ABORTED ? EAGAIN :
        op.error == ERROR_BROKEN_PIPE || op.error == ERROR_NO_DATA ? EPIPE : EIO;
    return -1;
}
static int md_win_fd_write_deadline(int fd, const void *buf, size_t len,
                                   int64_t deadline_ms, size_t *committed) {
    const char *p = buf;
    while (len) {
        int64_t remaining = deadline_ms - md_now_ms();
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        ssize_t n = win_fd_io(fd, (void *)p, len,
            remaining > INT_MAX ? INT_MAX : (int)remaining, true);
        if (n <= 0) {
            if (errno == EAGAIN) errno = ETIMEDOUT;
            return -1;
        }
        if (committed) *committed += (size_t)n;
        p += n; len -= (size_t)n;
    }
    return 0;
}
#endif

ssize_t md_socket_write(MdSocket fd, const void *buf, size_t len)
{
    int flags = 0;
#ifndef _WIN32
    flags = MSG_DONTWAIT;
#endif
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return md_socket_send(fd, buf, len, flags);
}

int md_fd_write_deadline(MdSocket fd, const void *buf, size_t len,
                         int64_t deadline_ms, bool is_socket, size_t *committed)
{
#ifdef _WIN32
    if (!is_socket) {
        if (fd < 0 || fd > INT_MAX) { errno = EBADF; return -1; }
        return md_win_fd_write_deadline((int)fd, buf, len, deadline_ms, committed);
    }
#endif
    const char *p = buf;
    size_t chunk = len;
    if (!is_socket) {
#ifndef _WIN32
        struct stat st;
        if (fstat(fd, &st) < 0) return -1;
        chunk = S_ISFIFO(st.st_mode) ? PIPE_BUF : 1;
#endif
    }
    while (len > 0) {
        int64_t remaining = deadline_ms - md_now_ms();
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        MdPollFd pfd = { .fd = fd, .events = POLLOUT };
        int rc = md_socket_poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
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

ssize_t md_fd_read_timeout(int fd, void *buf, size_t len, int timeout_ms) {
#ifdef _WIN32
    return win_fd_io(fd, buf, len, timeout_ms, false);
#else
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, timeout_ms);
    if (ready < 0) return -1;
    if (!ready) { errno = EAGAIN; return -1; }
    if (pfd.revents & POLLNVAL) { errno = EBADF; return -1; }
    return read(fd, buf, len);
#endif
}
