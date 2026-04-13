/*
 * metadesk — stream.c
 * TCP stream transport with MdPacketHeader framing.
 *
 * Uses POSIX sockets with blocking I/O. Each packet is sent as:
 *   [16-byte MdPacketHeader] [payload_len bytes of payload]
 *
 * Receive reads the header first, validates it, then reads the
 * payload in a loop until all bytes arrive (handles partial reads).
 *
 * Latency measurement: ping packets carry the send timestamp in
 * the header's timestamp_ms field. Pong echoes it back. RTT is
 * computed on pong receipt.
 */
#include "stream.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Structures ──────────────────────────────────────────────── */

struct MdStream {
    int          fd;
    bool         connected;
    uint32_t     send_seq;       /* monotonic sequence for sent packets    */

    /* Latency tracking */
    uint32_t     ping_send_ms;   /* timestamp when last ping was sent      */
    uint32_t     last_rtt_ms;
    uint32_t     avg_rtt_ms;     /* exponential moving average (alpha=1/8) */

    /* Counters */
    uint64_t     bytes_sent;
    uint64_t     bytes_recv;
    uint32_t     packets_sent;
    uint32_t     packets_recv;
};

struct MdStreamServer {
    int          fd;
    uint16_t     port;
};

/* ── Timestamp utility ───────────────────────────────────────── */

uint32_t md_stream_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ── Internal helpers ────────────────────────────────────────── */

/* Read exactly n bytes from fd, handling partial reads.
 * Returns 0 on success, -1 on error/EOF. */
static int read_exact(int fd, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    size_t total = 0;

    while (total < n) {
        if (timeout_ms > 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int pr = poll(&pfd, 1, (int)timeout_ms);
            if (pr == 0) return 1;   /* timeout */
            if (pr < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                return -1;
        }

        ssize_t r = read(fd, buf + total, n - total);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;  /* EOF or error */
        }
        total += (size_t)r;
    }
    return 0;
}

/* Write exactly n bytes to fd, handling partial writes. */
static int write_exact(int fd, const uint8_t *buf, size_t n) {
    size_t total = 0;

    while (total < n) {
        ssize_t w = write(fd, buf + total, n - total);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        total += (size_t)w;
    }
    return 0;
}

/* Set TCP_NODELAY to minimize latency (disable Nagle's algorithm). */
static void set_tcp_nodelay(int fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

/* Create an MdStream from a connected fd. */
static MdStream *stream_from_fd(int fd) {
    MdStream *s = calloc(1, sizeof(MdStream));
    if (!s) {
        close(fd);
        return NULL;
    }
    s->fd = fd;
    s->connected = true;
    set_tcp_nodelay(fd);
    return s;
}

/* ── Server API ──────────────────────────────────────────────── */

MdStreamServer *md_stream_server_create(const char *bind_addr, uint16_t port) {
    /* Create IPv6 socket with dual-stack (accepts IPv4 too) */
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;

    /* Allow address reuse */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Enable dual-stack: accept both IPv4 and IPv6 */
    int v6only = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);

    if (bind_addr) {
        if (inet_pton(AF_INET6, bind_addr, &addr.sin6_addr) != 1) {
            close(fd);
            return NULL;
        }
    } else {
        addr.sin6_addr = in6addr_any;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }

    if (listen(fd, 1) < 0) {
        close(fd);
        return NULL;
    }

    MdStreamServer *srv = calloc(1, sizeof(MdStreamServer));
    if (!srv) {
        close(fd);
        return NULL;
    }

    srv->fd = fd;
    srv->port = port;
    return srv;
}

MdStream *md_stream_server_accept(MdStreamServer *srv, uint32_t timeout_ms) {
    if (!srv) return NULL;

    if (timeout_ms > 0) {
        struct pollfd pfd = { .fd = srv->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, (int)timeout_ms);
        if (ret <= 0)
            return NULL;
    }

    struct sockaddr_in6 client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(srv->fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0)
        return NULL;

    char addr_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &client_addr.sin6_addr, addr_str, sizeof(addr_str));
    fprintf(stderr, "stream: accepted connection from %s\n", addr_str);

    return stream_from_fd(client_fd);
}

void md_stream_server_destroy(MdStreamServer *srv) {
    if (!srv) return;
    close(srv->fd);
    free(srv);
}

/* ── Client API ──────────────────────────────────────────────── */

MdStream *md_stream_connect(const char *host, uint16_t port, uint32_t timeout_ms) {
    if (!host) return NULL;

    /* Resolve hostname */
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return NULL;

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        /* Non-blocking connect with timeout */
        if (timeout_ms > 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            int ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
            if (ret < 0 && errno == EINPROGRESS) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                ret = poll(&pfd, 1, (int)timeout_ms);
                if (ret <= 0) {
                    close(fd);
                    fd = -1;
                    continue;
                }
                /* Check connect result */
                int err = 0;
                socklen_t errlen = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
                if (err != 0) {
                    close(fd);
                    fd = -1;
                    continue;
                }
            } else if (ret < 0) {
                close(fd);
                fd = -1;
                continue;
            }

            /* Restore blocking mode */
            fcntl(fd, F_SETFL, flags);
        } else {
            if (connect(fd, rp->ai_addr, rp->ai_addrlen) < 0) {
                close(fd);
                fd = -1;
                continue;
            }
        }

        break; /* connected */
    }

    freeaddrinfo(res);

    if (fd < 0)
        return NULL;

    return stream_from_fd(fd);
}

/* ── Stream I/O ──────────────────────────────────────────────── */

int md_stream_send(MdStream *s, uint8_t type, uint32_t seq,
                   const uint8_t *payload, uint32_t payload_len) {
    if (!s || !s->connected)
        return -1;

    /* Build header */
    uint8_t hdr_buf[MD_PACKET_HEADER_SIZE];
    MdPacketHeader hdr = {
        .version      = MD_PROTOCOL_VERSION,
        .type         = type,
        .flags        = 0,
        .payload_len  = payload_len,
        .sequence     = seq,
        .timestamp_ms = md_stream_now_ms(),
    };

    if (md_packet_header_write(&hdr, hdr_buf, sizeof(hdr_buf)) < 0)
        return -1;

    /* Send header */
    if (write_exact(s->fd, hdr_buf, MD_PACKET_HEADER_SIZE) < 0) {
        s->connected = false;
        return -1;
    }

    /* Send payload */
    if (payload && payload_len > 0) {
        if (write_exact(s->fd, payload, payload_len) < 0) {
            s->connected = false;
            return -1;
        }
    }

    s->bytes_sent += MD_PACKET_HEADER_SIZE + payload_len;
    s->packets_sent++;
    return 0;
}

int md_stream_recv(MdStream *s, MdPacketHeader *hdr,
                   uint8_t **payload_out, uint32_t timeout_ms) {
    if (!s || !s->connected || !hdr)
        return -1;

    /* Read header */
    uint8_t hdr_buf[MD_PACKET_HEADER_SIZE];
    int ret = read_exact(s->fd, hdr_buf, MD_PACKET_HEADER_SIZE, timeout_ms);
    if (ret != 0) {
        if (ret < 0) s->connected = false;
        return ret;  /* -1 = error, 1 = timeout */
    }

    if (md_packet_header_read(hdr, hdr_buf, sizeof(hdr_buf)) < 0) {
        s->connected = false;
        return -1;
    }

    /* Sanity check payload size */
    if (hdr->payload_len > MD_STREAM_MAX_PAYLOAD) {
        s->connected = false;
        return -1;
    }

    /* Read payload */
    uint8_t *payload = NULL;
    if (hdr->payload_len > 0) {
        payload = malloc(hdr->payload_len);
        if (!payload)
            return -1;

        ret = read_exact(s->fd, payload, hdr->payload_len, timeout_ms);
        if (ret != 0) {
            free(payload);
            if (ret < 0) s->connected = false;
            return ret;
        }
    }

    if (payload_out)
        *payload_out = payload;
    else
        free(payload);

    s->bytes_recv += (size_t)MD_PACKET_HEADER_SIZE + hdr->payload_len;
    s->packets_recv++;
    return 0;
}

/* ── Ping/Pong ───────────────────────────────────────────────── */

int md_stream_send_ping(MdStream *s) {
    if (!s) return -1;

    s->ping_send_ms = md_stream_now_ms();
    return md_stream_send(s, MD_PKT_PING, s->send_seq++, NULL, 0);
}

void md_stream_handle_pong(MdStream *s, const MdPacketHeader *hdr) {
    if (!s || !hdr) return;

    uint32_t now = md_stream_now_ms();
    uint32_t rtt = now - s->ping_send_ms;
    s->last_rtt_ms = rtt;

    /* Exponential moving average: avg = avg * 7/8 + rtt * 1/8 */
    if (s->avg_rtt_ms == 0)
        s->avg_rtt_ms = rtt;
    else
        s->avg_rtt_ms = (s->avg_rtt_ms * 7 + rtt) / 8;
}

/* ── Stats and utilities ─────────────────────────────────────── */

void md_stream_get_stats(const MdStream *s, MdStreamStats *stats) {
    if (!s || !stats) return;
    stats->last_rtt_ms  = s->last_rtt_ms;
    stats->avg_rtt_ms   = s->avg_rtt_ms;
    stats->bytes_sent   = s->bytes_sent;
    stats->bytes_recv   = s->bytes_recv;
    stats->packets_sent = s->packets_sent;
    stats->packets_recv = s->packets_recv;
}

int md_stream_get_fd(const MdStream *s) {
    return s ? s->fd : -1;
}

bool md_stream_is_connected(const MdStream *s) {
    return s ? s->connected : false;
}

void md_stream_destroy(MdStream *s) {
    if (!s) return;
    if (s->fd >= 0)
        close(s->fd);
    free(s);
}
