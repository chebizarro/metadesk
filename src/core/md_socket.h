/* Native sockets are not CRT file descriptors on Windows. */
#ifndef MD_SOCKET_H
#define MD_SOCKET_H
#include <stddef.h>
#include "md_socket_types.h"
#include <stdbool.h>
#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef WSAPOLLFD MdPollFd;
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
typedef struct pollfd MdPollFd;
#endif
#ifdef __cplusplus
extern "C" {
#endif
int md_socket_init(void);
MdSocket md_socket_open(int domain, int type, int protocol);
MdSocket md_socket_accept(MdSocket fd, struct sockaddr *addr, socklen_t *len);
int md_socket_close(MdSocket fd);
int md_socket_bind(MdSocket fd, const struct sockaddr *addr, socklen_t len);
int md_socket_listen(MdSocket fd, int backlog);
int md_socket_getname(MdSocket fd, struct sockaddr *addr, socklen_t *len);
int md_socket_shutdown(MdSocket fd, int how);
int md_socket_connect(MdSocket fd, const struct sockaddr *addr, socklen_t len);
int md_socket_poll(MdPollFd *fds, size_t count, int timeout_ms);
int md_socket_nonblocking(MdSocket fd, bool enabled);
ssize_t md_socket_send(MdSocket fd, const void *buf, size_t len, int flags);
ssize_t md_socket_recv(MdSocket fd, void *buf, size_t len, int flags);
int md_socket_setopt(MdSocket fd, int level, int name, const void *value, socklen_t len);
int md_socket_getopt(MdSocket fd, int level, int name, void *value, socklen_t *len);
/* POSIX address reuse; Windows exclusive ownership (SO_REUSEADDR is unsafe there). */
int md_socket_listener_reuse(MdSocket fd);
int md_socket_timeout(MdSocket fd, int name, int timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
