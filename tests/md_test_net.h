/*
 * metadesk — md_test_net.h
 * Deterministic waits for threaded test setup — replaces fixed
 * usleep()s that were either wasted wall-clock or flake surface.
 *
 * md_test_wait_readable: poll() until the fd has data or the deadline
 * expires. Returns 1 readable, 0 timeout, -1 poll error.
 */
#ifndef MD_TEST_NET_H
#define MD_TEST_NET_H

#include <errno.h>
#include <poll.h>
#include <unistd.h>

static int md_test_wait_readable(int fd, int timeout_ms)
{
    int waited = 0;
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP)))
            return 1;
        waited += 50;
        if (waited >= timeout_ms)
            return 0;
    }
}

#endif /* MD_TEST_NET_H */
