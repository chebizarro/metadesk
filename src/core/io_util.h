/* Shared monotonic deadlines and bounded transport writes. */
#ifndef MD_IO_UTIL_H
#define MD_IO_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "md_time.h" /* md_now_ms(): deadlines below are on its clock */

/* One nonblocking socket send, without changing the open file description.
 * On platforms without MSG_NOSIGNAL the socket must have SO_NOSIGPIPE set. */
ssize_t md_socket_write(int fd, const void *buf, size_t len);

/* Exclusive writer required. Never changes descriptor flags. Pipes use
 * <=PIPE_BUF writes after POLLOUT; other non-socket fds use one-byte writes
 * (PIPE_BUF readiness guarantees do not apply to terminals). Socket sends
 * are nonblocking per call. committed, when non-NULL, accumulates progress
 * even on failure, allowing callers to detect an incomplete frame. */
int md_fd_write_deadline(int fd, const void *buf, size_t len,
                         int64_t deadline_ms, bool is_socket, size_t *committed);

#endif
