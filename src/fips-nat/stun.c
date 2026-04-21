/*
 * fips-nat — stun.c
 * STUN address discovery (RFC 5389 Binding).
 *
 * Implements the STUN Binding transaction directly over UDP without
 * requiring libnice. The protocol is simple:
 *   1. Send 20-byte Binding Request (type + magic cookie + txn ID)
 *   2. Receive response with XOR-MAPPED-ADDRESS attribute
 *   3. XOR-decode the reflexive address and port
 *
 * Retries on timeout (UDP is unreliable). Supports both IPv4 and IPv6
 * STUN servers and reflexive addresses.
 *
 * Reference: RFC 5389 §§6-7, §15
 */
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Byte helpers (big-endian / network order) ───────────────── */

static inline uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static inline void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static inline void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

/* ── Random transaction ID ───────────────────────────────────── */

static void generate_txn_id(uint8_t txn_id[12]) {
    /* Use /dev/urandom on POSIX systems */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, txn_id, 12);
        close(fd);
        if (n == 12) return;
    }

    /* Fallback: seeded rand (not crypto-grade, but STUN txn IDs
     * are for matching requests to responses, not for security) */
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        seeded = 1;
    }
    for (int i = 0; i < 12; i++)
        txn_id[i] = (uint8_t)(rand() & 0xFF);
}

/* ── Build STUN Binding Request ──────────────────────────────── */

int md_stun_build_request(uint8_t *buf, size_t buf_len,
                          const uint8_t txn_id[12]) {
    if (!buf || buf_len < MD_STUN_HEADER_SIZE || !txn_id)
        return -1;

    /* Message Type: Binding Request (0x0001) */
    write_u16(buf + 0, MD_STUN_BINDING_REQUEST);
    /* Message Length: 0 (no attributes) */
    write_u16(buf + 2, 0);
    /* Magic Cookie */
    write_u32(buf + 4, MD_STUN_MAGIC_COOKIE);
    /* Transaction ID (12 bytes) */
    memcpy(buf + 8, txn_id, 12);

    return MD_STUN_HEADER_SIZE;
}

/* ── Parse STUN Binding Response ─────────────────────────────── */

/*
 * Decode an XOR-MAPPED-ADDRESS or MAPPED-ADDRESS attribute value.
 * For XOR-MAPPED-ADDRESS: port is XOR'd with cookie[0:2],
 *   IPv4 addr is XOR'd with cookie, IPv6 addr is XOR'd with cookie||txn_id.
 * For MAPPED-ADDRESS: no XOR.
 */
static int decode_mapped_addr(const uint8_t *val, uint16_t val_len,
                              const uint8_t txn_id[12],
                              bool xor_decode,
                              MdStunResult *result) {
    if (val_len < 4) return -1;

    /* val[0] is reserved, val[1] is family */
    uint8_t family = val[1];
    uint16_t port = read_u16(val + 2);

    if (xor_decode)
        port ^= (uint16_t)(MD_STUN_MAGIC_COOKIE >> 16);

    result->port = port;

    if (family == MD_STUN_FAMILY_IPV4) {
        if (val_len < 8) return -1;

        uint8_t addr[4];
        memcpy(addr, val + 4, 4);

        if (xor_decode) {
            uint32_t cookie = MD_STUN_MAGIC_COOKIE;
            addr[0] ^= (uint8_t)(cookie >> 24);
            addr[1] ^= (uint8_t)(cookie >> 16);
            addr[2] ^= (uint8_t)(cookie >>  8);
            addr[3] ^= (uint8_t)(cookie);
        }

        struct in_addr in;
        memcpy(&in, addr, 4);
        if (!inet_ntop(AF_INET, &in, result->ip, sizeof(result->ip)))
            return -1;

        result->is_ipv6 = false;
        return 0;

    } else if (family == MD_STUN_FAMILY_IPV6) {
        if (val_len < 20) return -1;

        uint8_t addr[16];
        memcpy(addr, val + 4, 16);

        if (xor_decode) {
            /* XOR with magic cookie (4 bytes) || transaction ID (12 bytes) */
            uint8_t xor_key[16];
            write_u32(xor_key, MD_STUN_MAGIC_COOKIE);
            memcpy(xor_key + 4, txn_id, 12);
            for (int i = 0; i < 16; i++)
                addr[i] ^= xor_key[i];
        }

        struct in6_addr in6;
        memcpy(&in6, addr, 16);
        if (!inet_ntop(AF_INET6, &in6, result->ip, sizeof(result->ip)))
            return -1;

        result->is_ipv6 = true;
        return 0;
    }

    return -1; /* unknown family */
}

int md_stun_parse_response(const uint8_t *buf, size_t buf_len,
                           const uint8_t txn_id[12],
                           MdStunResult *result) {
    if (!buf || !txn_id || !result)
        return -1;

    if (buf_len < MD_STUN_HEADER_SIZE)
        return -1;

    /* Validate header */
    uint16_t msg_type = read_u16(buf + 0);
    uint16_t msg_len  = read_u16(buf + 2);
    uint32_t cookie   = read_u32(buf + 4);

    /* Check for success response */
    if (msg_type != MD_STUN_BINDING_RESPONSE) {
        fprintf(stderr, "stun: unexpected message type 0x%04x\n", msg_type);
        return -1;
    }

    /* Validate magic cookie */
    if (cookie != MD_STUN_MAGIC_COOKIE) {
        fprintf(stderr, "stun: invalid magic cookie 0x%08x\n", cookie);
        return -1;
    }

    /* Validate transaction ID */
    if (memcmp(buf + 8, txn_id, 12) != 0) {
        fprintf(stderr, "stun: transaction ID mismatch\n");
        return -1;
    }

    /* Message length must not exceed buffer */
    if (MD_STUN_HEADER_SIZE + msg_len > buf_len) {
        fprintf(stderr, "stun: message truncated\n");
        return -1;
    }

    /* Walk attributes looking for XOR-MAPPED-ADDRESS or MAPPED-ADDRESS.
     * Prefer XOR-MAPPED-ADDRESS (§15.2) over MAPPED-ADDRESS (§15.1). */
    const uint8_t *attr_ptr = buf + MD_STUN_HEADER_SIZE;
    const uint8_t *end = attr_ptr + msg_len;

    bool found_mapped = false;
    MdStunResult mapped_result;
    memset(&mapped_result, 0, sizeof(mapped_result));

    while (attr_ptr + 4 <= end) {
        uint16_t attr_type = read_u16(attr_ptr + 0);
        uint16_t attr_len  = read_u16(attr_ptr + 2);
        const uint8_t *attr_val = attr_ptr + 4;

        /* Check attribute value fits in message */
        if (attr_val + attr_len > end)
            break;

        if (attr_type == MD_STUN_ATTR_XOR_MAPPED_ADDR) {
            /* Preferred: XOR-MAPPED-ADDRESS */
            if (decode_mapped_addr(attr_val, attr_len, txn_id,
                                   true, result) == 0)
                return 0;
        } else if (attr_type == MD_STUN_ATTR_MAPPED_ADDR) {
            /* Fallback: MAPPED-ADDRESS (no XOR) */
            if (decode_mapped_addr(attr_val, attr_len, txn_id,
                                   false, &mapped_result) == 0)
                found_mapped = true;
        }

        /* Attributes are padded to 4-byte boundaries */
        uint16_t padded_len = (attr_len + 3) & ~3u;
        attr_ptr = attr_val + padded_len;
    }

    /* Fall back to MAPPED-ADDRESS if XOR variant wasn't found */
    if (found_mapped) {
        *result = mapped_result;
        return 0;
    }

    fprintf(stderr, "stun: no MAPPED-ADDRESS attribute in response\n");
    return -1;
}

/* ── Network I/O ─────────────────────────────────────────────── */

/*
 * Parse "host:port" or just "host" from stun_server string.
 * Returns 0 on success.
 */
static int parse_server(const char *stun_server,
                        char *host_out, size_t host_len,
                        uint16_t *port_out) {
    if (!stun_server || !host_out || !port_out)
        return -1;

    /* Check for [IPv6]:port format */
    if (stun_server[0] == '[') {
        const char *bracket = strchr(stun_server, ']');
        if (!bracket) return -1;
        size_t len = (size_t)(bracket - stun_server - 1);
        if (len >= host_len) return -1;
        memcpy(host_out, stun_server + 1, len);
        host_out[len] = '\0';
        if (bracket[1] == ':')
            *port_out = (uint16_t)atoi(bracket + 2);
        return 0;
    }

    /* Check for host:port (only if there's exactly one colon — not IPv6) */
    const char *colon = strchr(stun_server, ':');
    if (colon && strchr(colon + 1, ':') == NULL) {
        /* single colon → host:port */
        size_t len = (size_t)(colon - stun_server);
        if (len >= host_len) return -1;
        memcpy(host_out, stun_server, len);
        host_out[len] = '\0';
        *port_out = (uint16_t)atoi(colon + 1);
        return 0;
    }

    /* Plain hostname or IPv6 without brackets */
    size_t len = strlen(stun_server);
    if (len >= host_len) return -1;
    memcpy(host_out, stun_server, len + 1);
    return 0;
}

int md_stun_discover_full(const char *stun_server, uint16_t stun_port,
                          MdStunResult *result, uint32_t timeout_ms) {
    if (!result)
        return -1;

    memset(result, 0, sizeof(*result));

    /* Defaults */
    char host[256];
    uint16_t port = stun_port ? stun_port : MD_STUN_DEFAULT_PORT;
    uint32_t timeout = timeout_ms ? timeout_ms : MD_STUN_DEFAULT_TIMEOUT;

    if (stun_server) {
        if (parse_server(stun_server, host, sizeof(host), &port) < 0) {
            fprintf(stderr, "stun: invalid server address: %s\n", stun_server);
            return -1;
        }
        /* If parse_server found a port in the string, use it.
         * If stun_port was explicitly provided, it takes precedence. */
        if (stun_port) port = stun_port;
    } else {
        strncpy(host, MD_STUN_DEFAULT_SERVER, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }

    /* Resolve STUN server */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int gai_ret = getaddrinfo(host, port_str, &hints, &res);
    if (gai_ret != 0) {
        fprintf(stderr, "stun: DNS resolution failed for %s: %s\n",
                host, gai_strerror(gai_ret));
        return -1;
    }

    /* Create UDP socket matching the server's address family */
    int fd = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        fprintf(stderr, "stun: socket() failed: %s\n", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    /* Generate transaction ID */
    uint8_t txn_id[12];
    generate_txn_id(txn_id);

    /* Build request */
    uint8_t req_buf[MD_STUN_HEADER_SIZE];
    int req_len = md_stun_build_request(req_buf, sizeof(req_buf), txn_id);
    if (req_len < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    /* Send with retries */
    uint8_t resp_buf[576]; /* RFC 5389 §7.1: STUN messages fit in 576 bytes */
    int ret = -1;

    for (int attempt = 0; attempt < MD_STUN_MAX_RETRIES; attempt++) {
        ssize_t sent = sendto(fd, req_buf, (size_t)req_len, 0,
                              res->ai_addr, res->ai_addrlen);
        if (sent != req_len) {
            fprintf(stderr, "stun: sendto() failed (attempt %d): %s\n",
                    attempt + 1, strerror(errno));
            continue;
        }

        /* Wait for response */
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)timeout);

        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "stun: poll() error: %s\n", strerror(errno));
            break;
        }

        if (pr == 0) {
            fprintf(stderr, "stun: timeout (attempt %d/%d)\n",
                    attempt + 1, MD_STUN_MAX_RETRIES);
            continue;
        }

        ssize_t recvd = recvfrom(fd, resp_buf, sizeof(resp_buf), 0, NULL, NULL);
        if (recvd < (ssize_t)MD_STUN_HEADER_SIZE) {
            fprintf(stderr, "stun: short response (%zd bytes)\n", recvd);
            continue;
        }

        /* Parse response */
        if (md_stun_parse_response(resp_buf, (size_t)recvd, txn_id, result) == 0) {
            ret = 0;
            break;
        }

        /* Parse failed — try again in case of spurious packet */
    }

    close(fd);
    freeaddrinfo(res);

    if (ret == 0) {
        fprintf(stderr, "stun: discovered reflexive address %s:%u%s\n",
                result->ip, result->port,
                result->is_ipv6 ? " (IPv6)" : "");
    }

    return ret;
}

/* ── Simple API (backward-compatible) ─────────���──────────────── */

int md_stun_discover(const char *stun_server, char *buf, int buf_len) {
    if (!buf || buf_len <= 0)
        return -1;

    MdStunResult result;
    if (md_stun_discover_full(stun_server, 0, &result, 0) < 0)
        return -1;

    int needed = snprintf(buf, (size_t)buf_len, "%s", result.ip);
    if (needed < 0 || needed >= buf_len)
        return -1;

    return 0;
}
