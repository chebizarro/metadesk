#include "md_socket.h"
#include <errno.h>
#include <limits.h>
#ifdef _WIN32
#include <pthread.h>
#include <stdlib.h>
static pthread_once_t wsa_once = PTHREAD_ONCE_INIT;
static int wsa_error;
static void wsa_cleanup(void) { WSACleanup(); }
static void wsa_start(void) {
    WSADATA data;
    wsa_error = WSAStartup(MAKEWORD(2, 2), &data);
    if (!wsa_error) atexit(wsa_cleanup);
}
static int socket_error(void) {
    switch (WSAGetLastError()) {
    case WSAEINTR: errno = EINTR; break;
    case WSAEWOULDBLOCK: errno = EWOULDBLOCK; break;
    case WSAEINPROGRESS: errno = EINPROGRESS; break;
    case WSAECONNRESET: case WSAECONNABORTED: errno = ECONNRESET; break;
    case WSAETIMEDOUT: errno = ETIMEDOUT; break;
    case WSAECONNREFUSED: errno = ECONNREFUSED; break;
    case WSAEADDRINUSE: errno = EADDRINUSE; break;
    case WSAENOTSOCK: errno = EBADF; break;
    case WSAEACCES: errno = EACCES; break;
    case WSAEINVAL: errno = EINVAL; break;
    default: errno = EIO; break;
    }
    return -1;
}
int md_socket_init(void) {
    int rc = pthread_once(&wsa_once, wsa_start);
    if (rc || wsa_error) { errno = EIO; return -1; }
    return 0;
}
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
int md_socket_init(void) { return 0; }
#endif
MdSocket md_socket_open(int domain, int type, int protocol) {
    if (md_socket_init()) return -1;
    MdSocket fd = (MdSocket)socket(domain, type, protocol);
#ifdef _WIN32
    if (fd == -1) socket_error();
#endif
    return fd;
}
MdSocket md_socket_accept(MdSocket fd, struct sockaddr *addr, socklen_t *len) {
    MdSocket result = (MdSocket)accept(fd, addr, len);
#ifdef _WIN32
    if (result == -1) socket_error();
#endif
    return result;
}
int md_socket_close(MdSocket fd) {
#ifdef _WIN32
    return closesocket((SOCKET)fd) == SOCKET_ERROR ? socket_error() : 0;
#else
    return close(fd);
#endif
}
int md_socket_connect(MdSocket fd, const struct sockaddr *addr, socklen_t len) {
    int rc = connect(fd, addr, len);
#ifdef _WIN32
    if (rc == SOCKET_ERROR) {
        socket_error();
        if (errno == EWOULDBLOCK) errno = EINPROGRESS;
    }
#endif
    return rc;
}
int md_socket_poll(MdPollFd *fds, size_t count, int timeout_ms) {
#ifdef _WIN32
    if (count > ULONG_MAX) { errno = EINVAL; return -1; }
    int rc = WSAPoll(fds, (ULONG)count, timeout_ms);
    return rc == SOCKET_ERROR ? socket_error() : rc;
#else
    return poll(fds, (nfds_t)count, timeout_ms);
#endif
}
int md_socket_nonblocking(MdSocket fd, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled;
    return ioctlsocket(fd, FIONBIO, &mode) == SOCKET_ERROR ? socket_error() : 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
}
ssize_t md_socket_send(MdSocket fd, const void *buf, size_t len, int flags) {
#ifdef _WIN32
    int rc = send(fd, (const char *)buf, len > INT_MAX ? INT_MAX : (int)len, flags);
    return rc == SOCKET_ERROR ? socket_error() : rc;
#else
    return send(fd, buf, len, flags);
#endif
}
ssize_t md_socket_recv(MdSocket fd, void *buf, size_t len, int flags) {
#ifdef _WIN32
    int rc = recv(fd, (char *)buf, len > INT_MAX ? INT_MAX : (int)len, flags);
    return rc == SOCKET_ERROR ? socket_error() : rc;
#else
    return recv(fd, buf, len, flags);
#endif
}
int md_socket_setopt(MdSocket fd, int level, int name, const void *value, socklen_t len) {
    int rc = setsockopt(fd, level, name, (const char *)value, len);
#ifdef _WIN32
    if (rc == SOCKET_ERROR) return socket_error();
#endif
    return rc;
}
int md_socket_getopt(MdSocket fd, int level, int name, void *value, socklen_t *len) {
    int rc = getsockopt(fd, level, name, (char *)value, len);
#ifdef _WIN32
    if (rc == SOCKET_ERROR) return socket_error();
#endif
    return rc;
}
int md_socket_timeout(MdSocket fd, int name, int timeout_ms) {
#ifdef _WIN32
    DWORD value = (DWORD)timeout_ms;
#else
    struct timeval value = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
#endif
    return md_socket_setopt(fd, SOL_SOCKET, name, &value, sizeof(value));
}

int md_socket_listener_reuse(MdSocket fd) {
    int enabled = 1;
#ifdef _WIN32
    return md_socket_setopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &enabled, sizeof(enabled));
#else
    return md_socket_setopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
}

int md_socket_bind(MdSocket fd, const struct sockaddr *addr, socklen_t len) {
    int rc = bind(fd, addr, len);
#ifdef _WIN32
    if (rc == SOCKET_ERROR) return socket_error();
#endif
    return rc;
}
int md_socket_listen(MdSocket fd, int backlog) {
    int rc = listen(fd, backlog);
#ifdef _WIN32
    if (rc == SOCKET_ERROR) return socket_error();
#endif
    return rc;
}
int md_socket_getname(MdSocket fd, struct sockaddr *addr, socklen_t *len) {
    int rc = getsockname(fd, addr, len);
#ifdef _WIN32
    if (rc == SOCKET_ERROR) return socket_error();
#endif
    return rc;
}
int md_socket_shutdown(MdSocket fd, int how) {
    int rc = shutdown(fd, how);
#ifdef _WIN32
    if (rc == SOCKET_ERROR) return socket_error();
#endif
    return rc;
}
