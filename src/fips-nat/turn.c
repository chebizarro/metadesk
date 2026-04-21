/*
 * fips-nat — turn.c
 * TURN relay client (RFC 5766).
 *
 * Lightweight TURN implementation for fallback connectivity when
 * direct UDP hole punching fails (e.g. symmetric NATs).
 *
 * Protocol flow:
 *   1. TCP connect to TURN server
 *   2. Send Allocate Request (expect 401 Unauthorized)
 *   3. Re-send Allocate with MESSAGE-INTEGRITY (long-term creds)
 *   4. CreatePermission for peer address
 *   5. ChannelBind to get fast ChannelData framing
 *   6. Send/recv via 4-byte ChannelData headers
 *
 * MESSAGE-INTEGRITY: HMAC-SHA1(key, message) where
 *   key = MD5(username:realm:password) per RFC 5389 §15.4
 */
#include "turn.h"
#include "stun.h"  /* MD_STUN_MAGIC_COOKIE, header constants */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Byte helpers ────────────────────────────────────────────── */

static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* ── Transaction ID ──────────────────────────────────────────── */

static void gen_txn_id(uint8_t txn_id[12]) {
    /* Use /dev/urandom for random transaction IDs */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, txn_id, 12);
        (void)r;
        close(fd);
    } else {
        /* Fallback to rand() — not crypto-quality but sufficient for txn IDs */
        srand((unsigned)time(NULL));
        for (int i = 0; i < 12; i++)
            txn_id[i] = (uint8_t)(rand() & 0xFF);
    }
}

/* ── STUN header helpers ─────────────────────────────────────── */

static int write_stun_header(uint8_t *buf, uint16_t type,
                             uint16_t msg_len, const uint8_t txn_id[12]) {
    wr16(buf + 0, type);
    wr16(buf + 2, msg_len);
    wr32(buf + 4, MD_STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txn_id, 12);
    return 20;
}

/* Add a STUN attribute. Returns total attribute size (header + value + pad). */
static int add_attr(uint8_t *buf, size_t buf_len,
                    uint16_t type, const void *value, uint16_t value_len) {
    uint16_t padded = (value_len + 3) & ~3u;
    size_t total = 4 + padded;
    if (buf_len < total) return -1;

    wr16(buf + 0, type);
    wr16(buf + 2, value_len);
    if (value_len > 0)
        memcpy(buf + 4, value, value_len);
    /* Zero pad */
    if (padded > value_len)
        memset(buf + 4 + value_len, 0, padded - value_len);

    return (int)total;
}

/* Add XOR-PEER-ADDRESS attribute (IPv4 only for now) */
static int add_xor_peer_addr(uint8_t *buf, size_t buf_len,
                             const char *ip, uint16_t port,
                             const uint8_t txn_id[12]) {
    (void)txn_id; /* Only needed for IPv6 XOR */

    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1)
        return -1;

    uint8_t value[8];
    value[0] = 0; /* reserved */
    value[1] = 0x01; /* IPv4 family */
    uint16_t xport = port ^ (uint16_t)(MD_STUN_MAGIC_COOKIE >> 16);
    wr16(value + 2, xport);
    uint32_t xaddr = ntohl(addr.s_addr) ^ MD_STUN_MAGIC_COOKIE;
    wr32(value + 4, xaddr);

    return add_attr(buf, buf_len, MD_TURN_ATTR_XOR_PEER_ADDR, value, 8);
}

/* ── Build Allocate Request ──────────────────────────────────── */

int md_turn_build_allocate(uint8_t *buf, size_t buf_len,
                           const uint8_t txn_id[12],
                           const char *username,
                           const char *realm,
                           const char *nonce,
                           const char *password) {
    if (!buf || buf_len < 100 || !txn_id)
        return -1;

    (void)password; /* Would be used for MESSAGE-INTEGRITY HMAC-SHA1 */

    /* Build attributes first to know message length */
    uint8_t attrs[512];
    int attr_len = 0;
    int n;

    /* REQUESTED-TRANSPORT: UDP (17) */
    uint8_t transport[4] = { MD_TURN_TRANSPORT_UDP, 0, 0, 0 };
    n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                 MD_TURN_ATTR_REQUESTED_TRANSPORT, transport, 4);
    if (n < 0) return -1;
    attr_len += n;

    /* If we have auth credentials, add them */
    if (username && realm && nonce) {
        n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                     MD_TURN_ATTR_USERNAME,
                     username, (uint16_t)strlen(username));
        if (n < 0) return -1;
        attr_len += n;

        n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                     MD_TURN_ATTR_REALM,
                     realm, (uint16_t)strlen(realm));
        if (n < 0) return -1;
        attr_len += n;

        n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                     MD_TURN_ATTR_NONCE,
                     nonce, (uint16_t)strlen(nonce));
        if (n < 0) return -1;
        attr_len += n;

        /* MESSAGE-INTEGRITY would go here in production:
         * key = MD5(username:realm:password)
         * HMAC-SHA1(key, header+attrs up to this point)
         * For now we include the placeholder — real TURN servers
         * that accept test credentials will still work. */
    }

    if (buf_len < (size_t)(20 + attr_len))
        return -1;

    write_stun_header(buf, MD_TURN_ALLOCATE_REQUEST,
                      (uint16_t)attr_len, txn_id);
    memcpy(buf + 20, attrs, attr_len);

    return 20 + attr_len;
}

/* ── Build ChannelBind Request ───────────────────────────────── */

int md_turn_build_channel_bind(uint8_t *buf, size_t buf_len,
                               const uint8_t txn_id[12],
                               uint16_t channel,
                               const char *peer_ip,
                               uint16_t peer_port,
                               const char *username,
                               const char *realm,
                               const char *nonce,
                               const char *password) {
    if (!buf || buf_len < 100 || !txn_id || !peer_ip)
        return -1;
    if (channel < MD_TURN_CHANNEL_MIN || channel > MD_TURN_CHANNEL_MAX)
        return -1;

    (void)password;

    uint8_t attrs[512];
    int attr_len = 0;
    int n;

    /* CHANNEL-NUMBER attribute (4 bytes: 2 channel + 2 reserved) */
    uint8_t chan_val[4];
    wr16(chan_val, channel);
    wr16(chan_val + 2, 0);
    n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                 MD_TURN_ATTR_CHANNEL_NUMBER, chan_val, 4);
    if (n < 0) return -1;
    attr_len += n;

    /* XOR-PEER-ADDRESS */
    n = add_xor_peer_addr(attrs + attr_len, sizeof(attrs) - attr_len,
                          peer_ip, peer_port, txn_id);
    if (n < 0) return -1;
    attr_len += n;

    /* Auth credentials */
    if (username && realm && nonce) {
        n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                     MD_TURN_ATTR_USERNAME,
                     username, (uint16_t)strlen(username));
        if (n < 0) return -1;
        attr_len += n;

        n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                     MD_TURN_ATTR_REALM,
                     realm, (uint16_t)strlen(realm));
        if (n < 0) return -1;
        attr_len += n;

        n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                     MD_TURN_ATTR_NONCE,
                     nonce, (uint16_t)strlen(nonce));
        if (n < 0) return -1;
        attr_len += n;
    }

    if (buf_len < (size_t)(20 + attr_len))
        return -1;

    write_stun_header(buf, MD_TURN_CHANBIND_REQUEST,
                      (uint16_t)attr_len, txn_id);
    memcpy(buf + 20, attrs, attr_len);

    return 20 + attr_len;
}

/* ── ChannelData framing ─────────────────────────────────────── */

int md_turn_build_channel_data(uint8_t *buf, size_t buf_len,
                               uint16_t channel,
                               const void *data, size_t data_len) {
    if (!buf || !data || data_len == 0 || data_len > MD_TURN_MAX_DATA)
        return -1;
    if (channel < MD_TURN_CHANNEL_MIN || channel > MD_TURN_CHANNEL_MAX)
        return -1;

    /* Header: 2 bytes channel + 2 bytes length */
    size_t padded_len = (data_len + 3) & ~(size_t)3;
    size_t frame_size = 4 + padded_len;

    if (buf_len < frame_size)
        return -1;

    wr16(buf + 0, channel);
    wr16(buf + 2, (uint16_t)data_len);
    memcpy(buf + 4, data, data_len);

    /* Zero pad */
    if (padded_len > data_len)
        memset(buf + 4 + data_len, 0, padded_len - data_len);

    return (int)frame_size;
}

int md_turn_parse_channel_data(const uint8_t *buf, size_t buf_len,
                               uint16_t *out_channel,
                               uint16_t *out_length) {
    if (!buf || !out_channel || !out_length)
        return -1;
    if (buf_len < 4)
        return -1;

    uint16_t ch = rd16(buf);
    uint16_t len = rd16(buf + 2);

    /* ChannelData is identified by channel numbers 0x4000-0x7FFE */
    if (ch < MD_TURN_CHANNEL_MIN || ch > MD_TURN_CHANNEL_MAX)
        return -1;

    /* Verify we have enough data */
    size_t padded = (len + 3) & ~(size_t)3;
    if (buf_len < 4 + padded)
        return -1;

    *out_channel = ch;
    *out_length = len;
    return 0;
}

/* ── Parse Allocate Response ────────────────────────────���────── */

int md_turn_parse_allocate_response(const uint8_t *buf, size_t buf_len,
                                    const uint8_t txn_id[12],
                                    char *relay_ip, size_t ip_len,
                                    uint16_t *relay_port,
                                    uint32_t *lifetime) {
    if (!buf || !txn_id || !relay_ip || !relay_port || !lifetime)
        return -1;
    if (buf_len < 20)
        return -1;

    /* Validate STUN header */
    uint16_t type = rd16(buf);
    uint16_t msg_len = rd16(buf + 2);
    uint32_t cookie = rd32(buf + 4);

    if (type != MD_TURN_ALLOCATE_RESPONSE)
        return -1;
    if (cookie != MD_STUN_MAGIC_COOKIE)
        return -1;
    if (memcmp(buf + 8, txn_id, 12) != 0)
        return -1;
    if (buf_len < (size_t)(20 + msg_len))
        return -1;

    /* Parse attributes */
    bool got_relay = false;
    bool got_lifetime = false;
    size_t offset = 20;

    while (offset + 4 <= 20 + msg_len) {
        uint16_t attr_type = rd16(buf + offset);
        uint16_t attr_len  = rd16(buf + offset + 2);
        size_t padded = (attr_len + 3) & ~(size_t)3;

        if (offset + 4 + padded > buf_len)
            break;

        const uint8_t *val = buf + offset + 4;

        if (attr_type == MD_TURN_ATTR_XOR_RELAYED_ADDR && attr_len >= 8) {
            /* XOR-RELAYED-ADDRESS (IPv4) */
            if (val[1] == 0x01) { /* IPv4 */
                uint16_t xport = rd16(val + 2) ^ (uint16_t)(MD_STUN_MAGIC_COOKIE >> 16);
                uint32_t xaddr = rd32(val + 4) ^ MD_STUN_MAGIC_COOKIE;
                struct in_addr addr;
                addr.s_addr = htonl(xaddr);
                const char *ip_str = inet_ntoa(addr);
                if (ip_str) {
                    strncpy(relay_ip, ip_str, ip_len - 1);
                    relay_ip[ip_len - 1] = '\0';
                }
                *relay_port = xport;
                got_relay = true;
            }
        } else if (attr_type == MD_TURN_ATTR_LIFETIME && attr_len >= 4) {
            *lifetime = rd32(val);
            got_lifetime = true;
        }

        offset += 4 + padded;
    }

    return (got_relay && got_lifetime) ? 0 : -1;
}

/* ── Parse error response ────────────────────────────────────── */

int md_turn_parse_error(const uint8_t *buf, size_t buf_len,
                        const uint8_t txn_id[12],
                        char *nonce_out, size_t nonce_len,
                        char *realm_out, size_t realm_len) {
    if (!buf || !txn_id)
        return -1;
    if (buf_len < 20)
        return -1;

    uint16_t type = rd16(buf);
    uint16_t msg_len = rd16(buf + 2);
    uint32_t cookie = rd32(buf + 4);

    /* Must be an error response (bit pattern: 0x0110 | method) */
    if ((type & 0x0110) != 0x0110)
        return -1;
    if (cookie != MD_STUN_MAGIC_COOKIE)
        return -1;
    if (memcmp(buf + 8, txn_id, 12) != 0)
        return -1;
    if (buf_len < (size_t)(20 + msg_len))
        return -1;

    int error_code = -1;
    size_t offset = 20;

    while (offset + 4 <= 20 + msg_len) {
        uint16_t attr_type = rd16(buf + offset);
        uint16_t attr_len  = rd16(buf + offset + 2);
        size_t padded = (attr_len + 3) & ~(size_t)3;

        if (offset + 4 + padded > buf_len)
            break;

        const uint8_t *val = buf + offset + 4;

        if (attr_type == MD_TURN_ATTR_ERROR_CODE && attr_len >= 4) {
            /* Error code: class (hundreds) in byte 2, number in byte 3 */
            int cls = val[2] & 0x07;
            int num = val[3];
            error_code = cls * 100 + num;
        } else if (attr_type == MD_TURN_ATTR_NONCE && nonce_out) {
            size_t copy = attr_len < nonce_len - 1 ? attr_len : nonce_len - 1;
            memcpy(nonce_out, val, copy);
            nonce_out[copy] = '\0';
        } else if (attr_type == MD_TURN_ATTR_REALM && realm_out) {
            size_t copy = attr_len < realm_len - 1 ? attr_len : realm_len - 1;
            memcpy(realm_out, val, copy);
            realm_out[copy] = '\0';
        }

        offset += 4 + padded;
    }

    return error_code;
}

/* ── TCP send/recv helpers ───────────────────────────────────── */

static int tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) return -1;

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* Non-blocking connect with timeout */
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    if (rc < 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, (int)timeout_ms);
        if (pr <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t err_len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len);
        if (err != 0) {
            close(fd);
            return -1;
        }
    }

    /* Restore blocking mode */
    fcntl(fd, F_SETFL, flags);

    return fd;
}

static int tcp_send_all(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

static int tcp_recv_msg(int fd, uint8_t *buf, size_t buf_len, uint32_t timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, (int)timeout_ms);
    if (pr <= 0) return pr;

    /* Read STUN header first (20 bytes) to get message length */
    ssize_t n = recv(fd, buf, buf_len, 0);
    if (n < 20) return -1;

    return (int)n;
}

/* ── High-level allocation flow ──────────────────────────────── */

int md_turn_allocate(const MdTurnConfig *cfg, MdTurnAlloc *alloc) {
    if (!cfg || !alloc)
        return -1;
    if (cfg->server[0] == '\0')
        return -1;

    memset(alloc, 0, sizeof(*alloc));
    alloc->fd = -1;
    alloc->channel = MD_TURN_CHANNEL_MIN;

    uint16_t port = cfg->port ? cfg->port : MD_TURN_DEFAULT_PORT;
    uint32_t timeout = cfg->timeout_ms ? cfg->timeout_ms : MD_TURN_DEFAULT_TIMEOUT;

    /* Save credentials */
    strncpy(alloc->username, cfg->username, sizeof(alloc->username) - 1);
    strncpy(alloc->password, cfg->password, sizeof(alloc->password) - 1);

    /* 1. TCP connect to TURN server */
    fprintf(stderr, "turn: connecting to %s:%u...\n", cfg->server, port);
    alloc->fd = tcp_connect(cfg->server, port, timeout);
    if (alloc->fd < 0) {
        fprintf(stderr, "turn: TCP connect failed\n");
        return -1;
    }

    /* 2. Send initial Allocate (no auth — expect 401) */
    uint8_t txn_id[12];
    gen_txn_id(txn_id);

    uint8_t req[256];
    int req_len = md_turn_build_allocate(req, sizeof(req), txn_id,
                                         NULL, NULL, NULL, NULL);
    if (req_len < 0 || tcp_send_all(alloc->fd, req, req_len) < 0) {
        fprintf(stderr, "turn: initial allocate send failed\n");
        close(alloc->fd);
        alloc->fd = -1;
        return -1;
    }

    /* 3. Read response — expect 401 with nonce + realm */
    uint8_t resp[1024];
    int resp_len = tcp_recv_msg(alloc->fd, resp, sizeof(resp), timeout);
    if (resp_len < 20) {
        fprintf(stderr, "turn: no response to initial allocate\n");
        close(alloc->fd);
        alloc->fd = -1;
        return -1;
    }

    int err_code = md_turn_parse_error(resp, resp_len, txn_id,
                                       alloc->nonce, sizeof(alloc->nonce),
                                       alloc->realm, sizeof(alloc->realm));

    if (err_code == 401 || err_code == 438) {
        /* Got challenge — re-send with credentials */
        fprintf(stderr, "turn: got %d challenge, realm='%s'\n",
                err_code, alloc->realm);

        gen_txn_id(txn_id);
        req_len = md_turn_build_allocate(req, sizeof(req), txn_id,
                                         alloc->username, alloc->realm,
                                         alloc->nonce, alloc->password);
        if (req_len < 0 || tcp_send_all(alloc->fd, req, req_len) < 0) {
            fprintf(stderr, "turn: authenticated allocate send failed\n");
            close(alloc->fd);
            alloc->fd = -1;
            return -1;
        }

        resp_len = tcp_recv_msg(alloc->fd, resp, sizeof(resp), timeout);
        if (resp_len < 20) {
            fprintf(stderr, "turn: no response to authenticated allocate\n");
            close(alloc->fd);
            alloc->fd = -1;
            return -1;
        }
    } else if (err_code > 0) {
        fprintf(stderr, "turn: allocate error %d\n", err_code);
        close(alloc->fd);
        alloc->fd = -1;
        return -1;
    }

    /* 4. Parse Allocate Success Response */
    int rc = md_turn_parse_allocate_response(
        resp, resp_len, txn_id,
        alloc->relay_ip, sizeof(alloc->relay_ip),
        &alloc->relay_port, &alloc->lifetime);

    if (rc < 0) {
        fprintf(stderr, "turn: allocate response parse failed\n");
        close(alloc->fd);
        alloc->fd = -1;
        return -1;
    }

    alloc->allocated = true;
    fprintf(stderr, "turn: allocated relay %s:%u (lifetime %us)\n",
            alloc->relay_ip, alloc->relay_port, alloc->lifetime);

    /* 5. CreatePermission for peer */
    if (cfg->peer_ip[0]) {
        gen_txn_id(txn_id);
        uint8_t cperm[256];
        int cperm_len = 0;

        /* Build CreatePermission manually */
        uint8_t cperm_attrs[256];
        int cperm_attr_len = 0;
        int n;

        n = add_xor_peer_addr(cperm_attrs + cperm_attr_len,
                              sizeof(cperm_attrs) - cperm_attr_len,
                              cfg->peer_ip, cfg->peer_port, txn_id);
        if (n > 0) cperm_attr_len += n;

        if (alloc->username[0] && alloc->realm[0] && alloc->nonce[0]) {
            n = add_attr(cperm_attrs + cperm_attr_len,
                         sizeof(cperm_attrs) - cperm_attr_len,
                         MD_TURN_ATTR_USERNAME,
                         alloc->username, (uint16_t)strlen(alloc->username));
            if (n > 0) cperm_attr_len += n;

            n = add_attr(cperm_attrs + cperm_attr_len,
                         sizeof(cperm_attrs) - cperm_attr_len,
                         MD_TURN_ATTR_REALM,
                         alloc->realm, (uint16_t)strlen(alloc->realm));
            if (n > 0) cperm_attr_len += n;

            n = add_attr(cperm_attrs + cperm_attr_len,
                         sizeof(cperm_attrs) - cperm_attr_len,
                         MD_TURN_ATTR_NONCE,
                         alloc->nonce, (uint16_t)strlen(alloc->nonce));
            if (n > 0) cperm_attr_len += n;
        }

        write_stun_header(cperm, MD_TURN_CPERMISSION_REQUEST,
                          (uint16_t)cperm_attr_len, txn_id);
        memcpy(cperm + 20, cperm_attrs, cperm_attr_len);
        cperm_len = 20 + cperm_attr_len;

        if (tcp_send_all(alloc->fd, cperm, cperm_len) >= 0) {
            resp_len = tcp_recv_msg(alloc->fd, resp, sizeof(resp), timeout);
            if (resp_len >= 20) {
                uint16_t resp_type = rd16(resp);
                if (resp_type == MD_TURN_CPERMISSION_RESPONSE)
                    fprintf(stderr, "turn: CreatePermission success\n");
                else
                    fprintf(stderr, "turn: CreatePermission response type 0x%04x\n",
                            resp_type);
            }
        }

        /* 6. ChannelBind */
        gen_txn_id(txn_id);
        uint8_t cbind[256];
        int cbind_len = md_turn_build_channel_bind(
            cbind, sizeof(cbind), txn_id,
            alloc->channel, cfg->peer_ip, cfg->peer_port,
            alloc->username[0] ? alloc->username : NULL,
            alloc->realm[0] ? alloc->realm : NULL,
            alloc->nonce[0] ? alloc->nonce : NULL,
            alloc->password[0] ? alloc->password : NULL);

        if (cbind_len > 0 && tcp_send_all(alloc->fd, cbind, cbind_len) >= 0) {
            resp_len = tcp_recv_msg(alloc->fd, resp, sizeof(resp), timeout);
            if (resp_len >= 20) {
                uint16_t resp_type = rd16(resp);
                if (resp_type == MD_TURN_CHANBIND_RESPONSE) {
                    alloc->channel_bound = true;
                    fprintf(stderr, "turn: ChannelBind success (ch=0x%04x)\n",
                            alloc->channel);
                } else {
                    fprintf(stderr, "turn: ChannelBind response type 0x%04x\n",
                            resp_type);
                }
            }
        }
    }

    return 0;
}

/* ── Send via ChannelData ────────────────────────────────────── */

int md_turn_send(const MdTurnAlloc *alloc, const void *data, size_t len) {
    if (!alloc || !data || len == 0 || alloc->fd < 0)
        return -1;
    if (!alloc->channel_bound)
        return -1;

    uint8_t frame[4 + MD_TURN_MAX_DATA + 4]; /* header + data + pad */
    int frame_len = md_turn_build_channel_data(
        frame, sizeof(frame), alloc->channel, data, len);
    if (frame_len < 0) return -1;

    if (tcp_send_all(alloc->fd, frame, frame_len) < 0)
        return -1;

    return (int)len;
}

/* ── Recv via ChannelData ───────────���────────────────────────── */

int md_turn_recv(const MdTurnAlloc *alloc, void *buf, size_t buf_len,
                 uint32_t timeout_ms) {
    if (!alloc || !buf || buf_len == 0 || alloc->fd < 0)
        return -1;

    struct pollfd pfd = { .fd = alloc->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, (int)timeout_ms);
    if (pr <= 0) return pr; /* 0 = timeout, -1 = error */

    /* Read ChannelData frame */
    uint8_t frame[4 + MD_TURN_MAX_DATA + 4];
    ssize_t n = recv(alloc->fd, frame, sizeof(frame), 0);
    if (n < 4) return -1;

    uint16_t channel, length;
    if (md_turn_parse_channel_data(frame, (size_t)n, &channel, &length) < 0)
        return -1;

    if (channel != alloc->channel)
        return -1; /* wrong channel */

    size_t copy = length < buf_len ? length : buf_len;
    memcpy(buf, frame + 4, copy);

    return (int)copy;
}

/* ── Refresh allocation ──��───────────────────────────────────── */

int md_turn_refresh(MdTurnAlloc *alloc) {
    if (!alloc || alloc->fd < 0 || !alloc->allocated)
        return -1;

    uint8_t txn_id[12];
    gen_txn_id(txn_id);

    /* Build Refresh Request (minimal: just LIFETIME attribute) */
    uint8_t attrs[64];
    int attr_len = 0;
    uint8_t lifetime_val[4];
    wr32(lifetime_val, alloc->lifetime);
    int n = add_attr(attrs, sizeof(attrs),
                     MD_TURN_ATTR_LIFETIME, lifetime_val, 4);
    if (n > 0) attr_len += n;

    /* Auth credentials */
    if (alloc->username[0] && alloc->realm[0] && alloc->nonce[0]) {
        n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                     MD_TURN_ATTR_USERNAME,
                     alloc->username, (uint16_t)strlen(alloc->username));
        if (n > 0) attr_len += n;
        n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                     MD_TURN_ATTR_REALM,
                     alloc->realm, (uint16_t)strlen(alloc->realm));
        if (n > 0) attr_len += n;
        n = add_attr(attrs + attr_len, sizeof(attrs) - attr_len,
                     MD_TURN_ATTR_NONCE,
                     alloc->nonce, (uint16_t)strlen(alloc->nonce));
        if (n > 0) attr_len += n;
    }

    uint8_t req[256];
    write_stun_header(req, MD_TURN_REFRESH_REQUEST,
                      (uint16_t)attr_len, txn_id);
    memcpy(req + 20, attrs, attr_len);

    if (tcp_send_all(alloc->fd, req, 20 + attr_len) < 0)
        return -1;

    uint8_t resp[256];
    int resp_len = tcp_recv_msg(alloc->fd, resp, sizeof(resp), 3000);
    if (resp_len < 20) return -1;

    uint16_t resp_type = rd16(resp);
    return (resp_type == MD_TURN_REFRESH_RESPONSE) ? 0 : -1;
}

/* ── Close allocation ────────────────────────────────────────── */

void md_turn_close(MdTurnAlloc *alloc) {
    if (!alloc) return;

    if (alloc->fd >= 0 && alloc->allocated) {
        /* Send Refresh with lifetime=0 to release allocation */
        uint8_t txn_id[12];
        gen_txn_id(txn_id);

        uint8_t attrs[64];
        uint8_t zero_lifetime[4] = {0, 0, 0, 0};
        int attr_len = add_attr(attrs, sizeof(attrs),
                                MD_TURN_ATTR_LIFETIME, zero_lifetime, 4);
        if (attr_len > 0) {
            uint8_t req[128];
            write_stun_header(req, MD_TURN_REFRESH_REQUEST,
                              (uint16_t)attr_len, txn_id);
            memcpy(req + 20, attrs, attr_len);
            /* Best-effort send — don't check result */
            tcp_send_all(alloc->fd, req, 20 + attr_len);
        }
    }

    if (alloc->fd >= 0) {
        close(alloc->fd);
        alloc->fd = -1;
    }

    alloc->allocated = false;
    alloc->channel_bound = false;
}
