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

/* TLS via OpenSSL */
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/evp.h>

/* ── Structures ──────────────────────────────────────────────── */

struct MdStream {
    int          fd;
    bool         connected;
    uint32_t     send_seq;       /* monotonic sequence for sent packets    */

    /* TLS (NULL when plaintext) */
    SSL         *ssl;

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
    SSL_CTX     *ssl_ctx;        /* NULL when plaintext */
};

/* ── Timestamp utility ───────────────────────────────────────── */

uint32_t md_stream_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ── Internal helpers ────────────────────────────────────────── */

static int ssl_retry_wait(int fd, int ssl_err, int timeout_ms) {
    short events;

    if (ssl_err == SSL_ERROR_WANT_READ) {
        events = POLLIN;
    } else if (ssl_err == SSL_ERROR_WANT_WRITE) {
        events = POLLOUT;
    } else {
        return -1;
    }

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = events };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) return 1;   /* timeout */
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
        return 0;
    }
}

/* Read exactly n bytes, handling partial reads.
 * Uses SSL_read when TLS is active, raw read() otherwise.
 * Returns 0 on success, -1 on error/EOF, 1 on timeout. */
static int read_exact(MdStream *s, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    size_t total = 0;
    int fd = s->fd;

    while (total < n) {
        /* For TLS streams, check SSL_pending before polling.
         * OpenSSL may have buffered data internally. */
        if (s->ssl && SSL_pending(s->ssl) > 0) {
            int r = SSL_read(s->ssl, buf + total, (int)(n - total));
            if (r <= 0) {
                int err = SSL_get_error(s->ssl, r);
                if (err == SSL_ERROR_ZERO_RETURN)
                    return -1;  /* clean TLS close */
                int wr = ssl_retry_wait(fd, err, timeout_ms > 0 ? (int)timeout_ms : -1);
                if (wr != 0) return wr;
                continue;
            }
            total += (size_t)r;
            continue;
        }

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

        if (s->ssl) {
            int r = SSL_read(s->ssl, buf + total, (int)(n - total));
            if (r <= 0) {
                int err = SSL_get_error(s->ssl, r);
                if (err == SSL_ERROR_ZERO_RETURN)
                    return -1;  /* clean TLS close */
                int wr = ssl_retry_wait(fd, err, timeout_ms > 0 ? (int)timeout_ms : -1);
                if (wr != 0) return wr;
                continue;
            }
            total += (size_t)r;
        } else {
            ssize_t r = read(fd, buf + total, n - total);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                return -1;
            }
            total += (size_t)r;
        }
    }
    return 0;
}

/* Write exactly n bytes, handling partial writes.
 * Uses SSL_write when TLS is active, raw write() otherwise. */
static int write_exact(MdStream *s, const uint8_t *buf, size_t n) {
    size_t total = 0;

    while (total < n) {
        if (s->ssl) {
            int w = SSL_write(s->ssl, buf + total, (int)(n - total));
            if (w <= 0) {
                int err = SSL_get_error(s->ssl, w);
                if (err == SSL_ERROR_ZERO_RETURN)
                    return -1;  /* clean TLS close */
                int wr = ssl_retry_wait(s->fd, err, -1);
                if (wr != 0) return -1;
                continue;
            }
            total += (size_t)w;
        } else {
            ssize_t w = write(s->fd, buf + total, n - total);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                return -1;
            }
            total += (size_t)w;
        }
    }
    return 0;
}

/* Set TCP_NODELAY to minimize latency (disable Nagle's algorithm). */
static void set_tcp_nodelay(int fd) {
    int flag = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0) {
        fprintf(stderr, "stream: warning: failed to set TCP_NODELAY: %s\n",
                strerror(errno));
    }
}

/* Create an MdStream from a connected fd (optionally with TLS). */
static MdStream *stream_from_fd(int fd, SSL *ssl) {
    MdStream *s = calloc(1, sizeof(MdStream));
    if (!s) {
        if (ssl) SSL_free(ssl);
        close(fd);
        return NULL;
    }
    s->fd = fd;
    s->ssl = ssl;
    s->connected = true;
    set_tcp_nodelay(fd);
    return s;
}

/* ── TLS helpers ─────────────────────────────────────────────── */

/* Generate a self-signed EC certificate + key for ephemeral TLS.
 * Used when the server has no cert_path configured. */
static SSL_CTX *tls_server_ctx_self_signed(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return NULL;

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);

    /* Generate ephemeral EC key (P-256) */
    EVP_PKEY *pkey = EVP_EC_gen("prime256v1");
    if (!pkey) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* Self-signed X509 certificate, valid for 24h */
    X509 *cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(pkey);
        SSL_CTX_free(ctx);
        return NULL;
    }

    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 86400L);
    X509_set_pubkey(cert, pkey);

    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (unsigned char *)"metadesk", -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509_sign(cert, pkey, EVP_sha256());

    SSL_CTX_use_certificate(ctx, cert);
    SSL_CTX_use_PrivateKey(ctx, pkey);

    X509_free(cert);
    EVP_PKEY_free(pkey);
    return ctx;
}

static SSL_CTX *tls_server_ctx_from_files(const char *cert_path,
                                          const char *key_path) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return NULL;

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "stream: failed to load TLS cert/key from %s / %s\n",
                cert_path, key_path);
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

static SSL_CTX *tls_client_ctx(bool verify_peer) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);

    if (verify_peer) {
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }
    return ctx;
}

/* ── Server API ──────────────────────────────────────────────── */

MdStreamServer *md_stream_server_create(const char *bind_addr, uint16_t port) {
    return md_stream_server_create_tls(bind_addr, port, NULL);
}

MdStreamServer *md_stream_server_create_tls(const char *bind_addr, uint16_t port,
                                            const MdStreamTlsConfig *tls) {
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

    /* Set up TLS context if requested */
    if (tls && tls->enabled) {
        if (tls->cert_path && tls->key_path) {
            srv->ssl_ctx = tls_server_ctx_from_files(tls->cert_path,
                                                     tls->key_path);
        } else {
            srv->ssl_ctx = tls_server_ctx_self_signed();
            if (srv->ssl_ctx)
                fprintf(stderr, "stream: using ephemeral self-signed TLS cert\n");
        }
        if (!srv->ssl_ctx) {
            fprintf(stderr, "stream: ERROR — failed to initialize TLS context\n");
            close(fd);
            free(srv);
            return NULL;
        }
        fprintf(stderr, "stream: TLS enabled (TLS 1.3)\n");
    }

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

    /* TLS handshake if server has a TLS context */
    SSL *ssl = NULL;
    if (srv->ssl_ctx) {
        ssl = SSL_new(srv->ssl_ctx);
        if (!ssl) {
            close(client_fd);
            return NULL;
        }
        SSL_set_fd(ssl, client_fd);
        if (SSL_accept(ssl) != 1) {
            fprintf(stderr, "stream: TLS handshake failed for %s\n", addr_str);
            SSL_free(ssl);
            close(client_fd);
            return NULL;
        }
        fprintf(stderr, "stream: TLS handshake complete (%s)\n",
                SSL_get_version(ssl));
    }

    return stream_from_fd(client_fd, ssl);
}

void md_stream_server_destroy(MdStreamServer *srv) {
    if (!srv) return;
    if (srv->ssl_ctx)
        SSL_CTX_free(srv->ssl_ctx);
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

    return stream_from_fd(fd, NULL);
}

MdStream *md_stream_connect_tls(const char *host, uint16_t port,
                                uint32_t timeout_ms,
                                const MdStreamTlsConfig *tls) {
    /* First establish the TCP connection */
    MdStream *s = md_stream_connect(host, port, timeout_ms);
    if (!s) return NULL;

    if (!tls || !tls->enabled)
        return s;

    /* Set up client TLS context */
    SSL_CTX *ctx = tls_client_ctx(tls->verify_peer);
    if (!ctx) {
        md_stream_destroy(s);
        return NULL;
    }

    SSL *ssl = SSL_new(ctx);
    SSL_CTX_free(ctx); /* SSL retains a ref; ctx can be freed */
    if (!ssl) {
        md_stream_destroy(s);
        return NULL;
    }

    SSL_set_fd(ssl, s->fd);
    /* Set SNI for certificate verification */
    SSL_set_tlsext_host_name(ssl, host);

    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "stream: TLS handshake failed connecting to %s:%u\n",
                host, port);
        SSL_free(ssl);
        md_stream_destroy(s);
        return NULL;
    }

    s->ssl = ssl;
    fprintf(stderr, "stream: TLS connected (%s)\n", SSL_get_version(ssl));
    return s;
}

/* ── FIPS-aware connect ──────────────────────────────────────── */

MdStream *md_stream_connect_fips(const char *npub, uint16_t port,
                                 uint32_t timeout_ms) {
    if (!npub || !md_fips_is_npub(npub))
        return NULL;

    /* Resolve npub → fd00::/8 IPv6 address.
     * md_fips_resolve tries DNS first (primes identity cache),
     * then falls back to direct computation. */
    char ipv6_str[MD_FIPS_IPV6_STRLEN];
    if (md_fips_resolve(npub, ipv6_str, sizeof(ipv6_str)) < 0) {
        fprintf(stderr, "stream: failed to resolve FIPS address for npub\n");
        return NULL;
    }

    fprintf(stderr, "stream: FIPS resolved %.*s... → %s\n",
            12, npub, ipv6_str);

    /* Connect using the resolved IPv6 address */
    return md_stream_connect(ipv6_str, port, timeout_ms);
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
    if (write_exact(s, hdr_buf, MD_PACKET_HEADER_SIZE) < 0) {
        s->connected = false;
        return -1;
    }

    /* Send payload */
    if (payload && payload_len > 0) {
        if (write_exact(s, payload, payload_len) < 0) {
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
    int ret = read_exact(s, hdr_buf, MD_PACKET_HEADER_SIZE, timeout_ms);
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

        ret = read_exact(s, payload, hdr->payload_len, timeout_ms);
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

bool md_stream_is_tls(const MdStream *s) {
    return s && s->ssl != NULL;
}

void md_stream_destroy(MdStream *s) {
    if (!s) return;
    if (s->ssl) {
        SSL_shutdown(s->ssl);
        SSL_free(s->ssl);
    }
    if (s->fd >= 0)
        close(s->fd);
    free(s);
}
