/*
 * fips-nat — punch.c
 * UDP hole punch coordinator.
 *
 * Implements simultaneous-open UDP hole punching:
 *   1. Bind a local UDP socket
 *   2. Send PROBE packets to the peer's reflexive address
 *   3. Listen for incoming PROBEs from the peer
 *   4. On receiving a PROBE, send back an ACK
 *   5. On receiving an ACK, the punch is confirmed (bidirectional)
 *
 * Both peers run this simultaneously. The PROBE creates NAT mappings;
 * the ACK confirms that the peer's PROBE created a return path.
 *
 * The session_id (16 bytes, exchanged via Nostr signaling) ensures
 * both sides are coordinating the same punch attempt.
 */
#include "punch.h"
#include "stun.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Byte helpers ────────────────────────────────────────────── */

static inline void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

static inline uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* ── Timestamp ───────────────────────────────────────────────── */

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ── Packet build / parse ────────────────────────────────────── */

int md_punch_build_packet(uint8_t *buf, size_t buf_len,
                          uint8_t type, const uint8_t session_id[16]) {
    if (!buf || buf_len < MD_PUNCH_PACKET_SIZE || !session_id)
        return -1;
    if (type != MD_PUNCH_TYPE_PROBE && type != MD_PUNCH_TYPE_ACK)
        return -1;

    memset(buf, 0, MD_PUNCH_PACKET_SIZE);
    write_u32(buf + 0, MD_PUNCH_MAGIC);     /* magic: "MDPH" */
    buf[4] = type;                            /* type          */
    buf[5] = MD_PUNCH_VERSION;                /* version       */
    /* buf[6..7] reserved */
    memcpy(buf + 8, session_id, 16);          /* session ID    */

    return MD_PUNCH_PACKET_SIZE;
}

int md_punch_parse_packet(const uint8_t *buf, size_t buf_len,
                          const uint8_t session_id[16],
                          uint8_t *out_type) {
    if (!buf || !session_id || !out_type)
        return -1;
    if (buf_len < MD_PUNCH_PACKET_SIZE)
        return -1;

    /* Validate magic */
    uint32_t magic = read_u32(buf + 0);
    if (magic != MD_PUNCH_MAGIC)
        return -1;

    /* Validate version */
    if (buf[5] != MD_PUNCH_VERSION)
        return -1;

    /* Validate type */
    uint8_t type = buf[4];
    if (type != MD_PUNCH_TYPE_PROBE && type != MD_PUNCH_TYPE_ACK)
        return -1;

    /* Validate session ID */
    if (memcmp(buf + 8, session_id, 16) != 0)
        return -1;

    *out_type = type;
    return 0;
}

/* ── Resolve peer address to sockaddr ────────────────────────── */

static int resolve_peer(const char *ip, uint16_t port,
                        struct sockaddr_storage *addr,
                        socklen_t *addr_len) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags    = AI_NUMERICHOST;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int ret = getaddrinfo(ip, port_str, &hints, &res);
    if (ret != 0) return -1;

    memcpy(addr, res->ai_addr, res->ai_addrlen);
    *addr_len = res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

/* ── Main punch loop ─────────────────────────────────────────── */

int md_punch_execute(const MdPunchConfig *cfg, MdPunchResult *result) {
    if (!cfg || !result)
        return -1;
    if (cfg->peer_ip[0] == '\0' || cfg->peer_port == 0)
        return -1;

    memset(result, 0, sizeof(*result));
    result->fd = -1;

    /* Apply defaults */
    uint32_t timeout    = cfg->timeout_ms       ? cfg->timeout_ms
                                                 : MD_PUNCH_DEFAULT_TIMEOUT;
    uint32_t interval   = cfg->probe_interval_ms ? cfg->probe_interval_ms
                                                  : MD_PUNCH_DEFAULT_INTERVAL;

    /* Resolve peer address */
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;
    if (resolve_peer(cfg->peer_ip, cfg->peer_port,
                     &peer_addr, &peer_addr_len) < 0) {
        fprintf(stderr, "punch: failed to resolve peer %s:%u\n",
                cfg->peer_ip, cfg->peer_port);
        return -1;
    }

    /* Create UDP socket matching peer's address family */
    int af = peer_addr.ss_family;
    int fd = socket(af, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        fprintf(stderr, "punch: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    /* Bind to requested local port (or ephemeral) */
    if (af == AF_INET6) {
        struct sockaddr_in6 bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin6_family = AF_INET6;
        bind_addr.sin6_port = htons(cfg->local_bind_port);
        bind_addr.sin6_addr = in6addr_any;
        if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            fprintf(stderr, "punch: bind() failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    } else {
        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(cfg->local_bind_port);
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            fprintf(stderr, "punch: bind() failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }

    /* Discover our actual bound port */
    struct sockaddr_storage local_addr;
    socklen_t local_len = sizeof(local_addr);
    if (getsockname(fd, (struct sockaddr *)&local_addr, &local_len) == 0) {
        if (af == AF_INET6)
            result->local_port = ntohs(((struct sockaddr_in6 *)&local_addr)->sin6_port);
        else
            result->local_port = ntohs(((struct sockaddr_in *)&local_addr)->sin_port);
    }

    fprintf(stderr, "punch: starting hole punch to %s:%u (local port %u, timeout %ums)\n",
            cfg->peer_ip, cfg->peer_port, result->local_port, timeout);

    /* Build probe and ack packets */
    uint8_t probe_pkt[MD_PUNCH_PACKET_SIZE];
    uint8_t ack_pkt[MD_PUNCH_PACKET_SIZE];
    md_punch_build_packet(probe_pkt, sizeof(probe_pkt),
                          MD_PUNCH_TYPE_PROBE, cfg->session_id);
    md_punch_build_packet(ack_pkt, sizeof(ack_pkt),
                          MD_PUNCH_TYPE_ACK, cfg->session_id);

    /* State machine */
    MdPunchState state = MD_PUNCH_STATE_INIT;
    uint32_t start_ms     = now_ms();
    uint32_t last_probe   = 0;
    uint32_t probe_sent   = start_ms; /* timestamp for RTT */
    int      ret          = -1;

    state = MD_PUNCH_STATE_PROBING;

    while (now_ms() - start_ms < timeout) {
        uint32_t now = now_ms();

        /* Send probe at regular intervals */
        if (now - last_probe >= interval) {
            ssize_t sent = sendto(fd, probe_pkt, MD_PUNCH_PACKET_SIZE, 0,
                                  (struct sockaddr *)&peer_addr, peer_addr_len);
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                /* ICMP unreachable is OK — NAT hasn't opened yet */
                if (errno != ECONNREFUSED && errno != ENETUNREACH &&
                    errno != EHOSTUNREACH) {
                    fprintf(stderr, "punch: sendto() error: %s\n",
                            strerror(errno));
                }
            }
            probe_sent = now;
            last_probe = now;
        }

        /* Wait for incoming packet (short poll to allow re-probing) */
        uint32_t remaining = interval - (now - last_probe);
        if (remaining > interval) remaining = interval;
        if (remaining == 0) remaining = 1;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)remaining);

        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "punch: poll() error: %s\n", strerror(errno));
            break;
        }

        if (pr == 0)
            continue; /* timeout — loop back to send next probe */

        /* Read incoming packet */
        uint8_t recv_buf[MD_PUNCH_PACKET_SIZE + 16]; /* extra for safety */
        struct sockaddr_storage from_addr;
        socklen_t from_len = sizeof(from_addr);

        ssize_t recvd = recvfrom(fd, recv_buf, sizeof(recv_buf), 0,
                                 (struct sockaddr *)&from_addr, &from_len);
        if (recvd < (ssize_t)MD_PUNCH_PACKET_SIZE)
            continue; /* too short — ignore */

        uint8_t pkt_type;
        if (md_punch_parse_packet(recv_buf, (size_t)recvd,
                                  cfg->session_id, &pkt_type) < 0)
            continue; /* invalid or wrong session — ignore */

        if (pkt_type == MD_PUNCH_TYPE_PROBE) {
            /* Peer's probe arrived — our NAT path is open inbound.
             * Send ACK to confirm we received it. */
            sendto(fd, ack_pkt, MD_PUNCH_PACKET_SIZE, 0,
                   (struct sockaddr *)&from_addr, from_len);

            if (state == MD_PUNCH_STATE_PROBING)
                state = MD_PUNCH_STATE_GOT_PROBE;

            fprintf(stderr, "punch: received PROBE from peer\n");

        } else if (pkt_type == MD_PUNCH_TYPE_ACK) {
            /* Peer acknowledged our probe — bidirectional path confirmed! */
            state = MD_PUNCH_STATE_CONFIRMED;
            result->rtt_ms = now_ms() - probe_sent;
            fprintf(stderr, "punch: received ACK — hole punch confirmed (RTT ~%ums)\n",
                    result->rtt_ms);
            break;
        }
    }

    if (state == MD_PUNCH_STATE_CONFIRMED) {
        /* "Connect" the UDP socket to the peer for convenience.
         * This allows using send()/recv() instead of sendto()/recvfrom(). */
        if (connect(fd, (struct sockaddr *)&peer_addr, peer_addr_len) < 0) {
            fprintf(stderr, "punch: connect() failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }

        result->fd = fd;
        strncpy(result->peer_ip, cfg->peer_ip, sizeof(result->peer_ip) - 1);
        result->peer_port = cfg->peer_port;
        ret = 0;

        fprintf(stderr, "punch: success — connected UDP socket to %s:%u\n",
                result->peer_ip, result->peer_port);
    } else {
        fprintf(stderr, "punch: failed (state=%d, elapsed=%ums)\n",
                state, now_ms() - start_ms);
        close(fd);
        ret = -1;
    }

    return ret;
}

/* ── Simple legacy API ───────────────────────────────────────── */

int md_punch_peer(const char *peer_addr, int peer_port) {
    if (!peer_addr || peer_port <= 0 || peer_port > 65535)
        return -1;

    MdPunchConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.peer_ip, peer_addr, sizeof(cfg.peer_ip) - 1);
    cfg.peer_port = (uint16_t)peer_port;

    /* Generate a random session ID for the simple API */
    int urand = open("/dev/urandom", O_RDONLY);
    if (urand >= 0) {
        read(urand, cfg.session_id, 16);
        close(urand);
    }

    MdPunchResult result;
    if (md_punch_execute(&cfg, &result) < 0)
        return -1;

    return result.fd;
}
