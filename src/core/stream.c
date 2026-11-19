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
 * Latency measurement: md_stream_send_ping() records its send time
 * locally (ping_send_ms) and only one ping is tracked in flight; on
 * pong receipt RTT = now - ping_send_ms, fed into an EMA. The pong's
 * own header timestamp is not used (no cross-peer echo protocol).
 *
 * Threading: one sender and one receiver may use a stream concurrently (the
 * host sends video from its capture thread while the main thread receives).
 * The fields both directions touch — connected and the traffic counters — are
 * atomics (md-9sa6); everything else belongs to one side or to the caller's
 * lock.
 *
 * TLS (md-z6nw): OpenSSL forbids two threads in one SSL object, and both
 * directions drive the same one. After the handshake a TLS stream's socket is
 * non-blocking, and ssl_mu covers each SSL_read/SSL_write together with its
 * SSL_get_error, nothing more: on SSL_ERROR_WANT_READ/WANT_WRITE the lock is
 * dropped before poll(), so a receiver waiting for data never holds off a
 * sender, and a sender waiting for socket space never holds off a receiver.
 * Plain TCP stays blocking and never takes the lock.
 */
#include "stream.h"
#include "md_time.h"

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
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* TLS via OpenSSL */
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include "log.h"
#define MD_LOG_TAG "stream"

/* ── Structures ──────────────────────────────────────────────── */

/* Relaxed atomics: each is a standalone flag or counter that publishes no
 * other data, so only the data race needs removing, not ordering added. */
#define RLX_LOAD(field) atomic_load_explicit(&(field), memory_order_relaxed)
#define RLX_STORE(field, v) atomic_store_explicit(&(field), (v), memory_order_relaxed)
#define RLX_ADD(field, v) atomic_fetch_add_explicit(&(field), (v), memory_order_relaxed)

struct MdStream {
    int          fd;
    atomic_bool  connected;      /* cleared by whichever direction fails */
    bool         session_bound;
    uint32_t     granted_caps;
    uint32_t     send_seq;       /* monotonic sequence for sent packets    */

    /* TLS (NULL when plaintext). ssl_mu serialises every call into ssl and
     * is never held across a wait (md-z6nw). */
    SSL         *ssl;
    pthread_mutex_t ssl_mu;

    /* Latency tracking */
    uint32_t     ping_send_ms;   /* timestamp when last ping was sent      */
    uint32_t     last_rtt_ms;
    uint32_t     avg_rtt_ms;     /* exponential moving average (alpha=1/8) */

    /* Counters: each written by one direction, read by md_stream_get_stats */
    _Atomic uint64_t bytes_sent;
    _Atomic uint64_t bytes_recv;
    _Atomic uint32_t packets_sent;
    _Atomic uint32_t packets_recv;
};

struct MdStreamServer {
    int          fd;
    uint16_t     port;
    SSL_CTX     *ssl_ctx;        /* NULL when plaintext */
};

/* ── Timestamp utility ───────────────────────────────────────── */

uint32_t md_stream_now_ms(void) {
    return (uint32_t)md_now_ms();
}

/* ── Internal helpers ────────────────────────────────────────── */

/* Wait, without ssl_mu, until the socket can satisfy an SSL_ERROR_WANT_*.
 * Returns 0 when ready, 1 on timeout, -1 on error or another SSL error. */
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
            /* Not a restart with the full budget: the caller retries the
             * SSL call and recomputes what is left of its deadline. */
            if (errno == EINTR) return 0;
            return -1;
        }
        /* Readable-at-hangup still holds the peer's last records (and its
         * close_notify); the next SSL call reports the end. */
        if (!(pfd.revents & POLLNVAL) && (pfd.revents & events))
            return 0;
        return -1;
    }
}

/* One SSL_read or SSL_write on a TLS stream, holding ssl_mu only for the
 * call and its SSL_get_error. The socket is non-blocking, so neither blocks.
 * Returns the bytes moved, or <= 0 with *ssl_err set. */
static int ssl_io(MdStream *s, bool write, void *buf, size_t len, int *ssl_err) {
    const int n = len > INT_MAX ? INT_MAX : (int)len;
    pthread_mutex_lock(&s->ssl_mu);
    ERR_clear_error(); /* SSL_get_error reads this thread's error queue */
    const int r = write ? SSL_write(s->ssl, buf, n) : SSL_read(s->ssl, buf, n);
    if (r <= 0) *ssl_err = SSL_get_error(s->ssl, r);
    pthread_mutex_unlock(&s->ssl_mu);
    return r;
}

/* Read exactly n bytes, handling partial reads.
 * Uses SSL_read when TLS is active (see ssl_io), raw read() otherwise.
 * timeout_ms is a deadline for the whole read, not a per-syscall
 * budget — a peer dribbling bytes cannot extend it.
 * Returns 0 on success, -1 on error/EOF, 1 on timeout. */
static int read_exact(MdStream *s, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    size_t total = 0;
    int fd = s->fd;
    const uint32_t deadline = timeout_ms > 0
        ? md_stream_now_ms() + timeout_ms : 0;

    while (total < n) {
        int wait_ms = -1;
        if (deadline) {
            uint32_t now = md_stream_now_ms();
            if (now >= deadline)
                return 1;   /* timeout */
            wait_ms = (int)(deadline - now);
        }

        /* TLS: try first (OpenSSL may hold buffered records), then wait for
         * whatever the record layer asked for, with ssl_mu released. */
        if (s->ssl) {
            int err = SSL_ERROR_NONE;
            int r = ssl_io(s, false, buf + total, n - total, &err);
            if (r > 0) {
                total += (size_t)r;
                continue;
            }
            if (err == SSL_ERROR_ZERO_RETURN)
                return -1;  /* clean TLS close */
            int wr = ssl_retry_wait(fd, err, wait_ms);
            if (wr != 0) return wr;
            continue;
        }

        if (wait_ms >= 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int pr = poll(&pfd, 1, wait_ms);
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
            return -1;
        }
        total += (size_t)r;
    }
    return 0;
}

/* A TLS 1.3 write (every context here pins 1.3) never waits on a read once
 * the handshake is done. If one ever does, the receiver may consume the
 * bytes it waits for, so it re-tries the write after this slice rather
 * than wait on POLLIN for good. */
#define SSL_WRITE_WANT_READ_SLICE_MS 50

/* Write exactly n bytes, handling partial writes.
 * Uses SSL_write when TLS is active (see ssl_io), raw write() otherwise. */
static int write_exact(MdStream *s, const uint8_t *buf, size_t n) {
    size_t total = 0;

    while (total < n) {
        if (s->ssl) {
            /* A retry after WANT_WRITE passes the same buf + total and length,
             * as SSL_write requires: total only moves on success. */
            int err = SSL_ERROR_NONE;
            int w = ssl_io(s, true, (void *)(buf + total), n - total, &err);
            if (w <= 0) {
                if (err == SSL_ERROR_ZERO_RETURN)
                    return -1;  /* clean TLS close */
                const int slice = err == SSL_ERROR_WANT_READ
                    ? SSL_WRITE_WANT_READ_SLICE_MS : -1;
                if (ssl_retry_wait(s->fd, err, slice) < 0) return -1;
                continue;
            }
            total += (size_t)w;
        } else {
            /* A peer that closed mid-send must surface as EPIPE, not kill
             * the process: MSG_NOSIGNAL on Linux, SO_NOSIGPIPE (set in
             * stream_from_fd) on macOS/BSD. */
#ifdef MSG_NOSIGNAL
            ssize_t w = send(s->fd, buf + total, n - total, MSG_NOSIGNAL);
#else
            ssize_t w = write(s->fd, buf + total, n - total);
#endif
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
        MD_LOG_W("warning: failed to set TCP_NODELAY: %s", strerror(errno));
    }
}

/* After a blocking handshake, a TLS socket goes non-blocking so that
 * ssl_io never blocks while holding ssl_mu (see the note at the top). */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

/* Create an MdStream from a connected fd (optionally with TLS, its handshake
 * done and fd already non-blocking). */
static MdStream *stream_from_fd(int fd, SSL *ssl) {
    MdStream *s = calloc(1, sizeof(MdStream));
    if (!s || pthread_mutex_init(&s->ssl_mu, NULL) != 0) {
        free(s);
        if (ssl) SSL_free(ssl);
        close(fd);
        return NULL;
    }
    s->fd = fd;
    s->ssl = ssl;
    atomic_init(&s->connected, true);
    set_tcp_nodelay(fd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
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

    X509_NAME *name = X509_get_subject_name(cert);

    /* A failure anywhere here yields an unsigned or keyless cert that would
     * make every TLS handshake fail with an opaque error; bail loudly. */
    if (!ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) ||
        !X509_gmtime_adj(X509_get_notBefore(cert), 0) ||
        !X509_gmtime_adj(X509_get_notAfter(cert), 86400L) ||
        !X509_set_pubkey(cert, pkey) ||
        !X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    (unsigned char *)"metadesk", -1, -1, 0) ||
        !X509_set_issuer_name(cert, name) ||
        !X509_sign(cert, pkey, EVP_sha256()) ||
        SSL_CTX_use_certificate(ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
        MD_LOG_E("failed to build self-signed TLS certificate");
        X509_free(cert);
        EVP_PKEY_free(pkey);
        SSL_CTX_free(ctx);
        return NULL;
    }

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
        MD_LOG_E("failed to load TLS cert/key from %s / %s", cert_path, key_path);
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
            struct in_addr v4_addr;
            if (inet_pton(AF_INET, bind_addr, &v4_addr) == 1) {
                memset(&addr.sin6_addr, 0, sizeof(addr.sin6_addr));
                addr.sin6_addr.s6_addr[10] = 0xff;
                addr.sin6_addr.s6_addr[11] = 0xff;
                memcpy(&addr.sin6_addr.s6_addr[12], &v4_addr, sizeof(v4_addr));
            } else {
                MD_LOG_E("invalid TCP bind address '%s': expected an IPv4 or IPv6 literal",
                         bind_addr);
                close(fd);
                errno = EINVAL;
                return NULL;
            }
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
    socklen_t bound_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &bound_len) != 0) {
        close(fd);
        free(srv);
        return NULL;
    }
    srv->port = ntohs(addr.sin6_port);

    /* Set up TLS context if requested */
    if (tls && tls->enabled) {
        if (tls->cert_path && tls->key_path) {
            srv->ssl_ctx = tls_server_ctx_from_files(tls->cert_path,
                                                     tls->key_path);
        } else {
            srv->ssl_ctx = tls_server_ctx_self_signed();
            if (srv->ssl_ctx)
                MD_LOG_I("using ephemeral self-signed TLS cert");
        }
        if (!srv->ssl_ctx) {
            MD_LOG_E("ERROR — failed to initialize TLS context");
            close(fd);
            free(srv);
            return NULL;
        }
        MD_LOG_I("TLS enabled (TLS 1.3)");
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
    MD_LOG_I("accepted connection from %s", addr_str);

    /* TLS handshake if server has a TLS context */
    SSL *ssl = NULL;
    if (srv->ssl_ctx) {
        ssl = SSL_new(srv->ssl_ctx);
        if (!ssl) {
            close(client_fd);
            return NULL;
        }
        SSL_set_fd(ssl, client_fd);
        if (SSL_accept(ssl) != 1 || set_nonblocking(client_fd) != 0) {
            MD_LOG_E("TLS handshake failed for %s", addr_str);
            SSL_free(ssl);
            close(client_fd);
            return NULL;
        }
        MD_LOG_I("TLS handshake complete (%s)", SSL_get_version(ssl));
    }

    return stream_from_fd(client_fd, ssl);
}

uint16_t md_stream_server_port(const MdStreamServer *srv) {
    return srv ? srv->port : 0;
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
    /* SNI for name-based virtual hosting */
    SSL_set_tlsext_host_name(ssl, host);
    /* Verify the certificate matches the host we dialed (SNI alone
     * does no verification) */
    if (tls->verify_peer)
        SSL_set1_host(ssl, host);

    if (SSL_connect(ssl) != 1 || set_nonblocking(s->fd) != 0) {
        MD_LOG_E("TLS handshake failed connecting to %s:%u", host, port);
        SSL_free(ssl);
        md_stream_destroy(s);
        return NULL;
    }

    s->ssl = ssl;
    MD_LOG_I("TLS connected (%s)", SSL_get_version(ssl));
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
        MD_LOG_E("failed to resolve FIPS address for npub");
        return NULL;
    }

    MD_LOG_I("FIPS resolved %.*s... → %s", 12, npub, ipv6_str);

    /* Connect using the resolved IPv6 address */
    return md_stream_connect(ipv6_str, port, timeout_ms);
}

/* ── Stream I/O ──────────────────────────────────────────────── */

int md_stream_send(MdStream *s, uint8_t type, uint32_t seq,
                   const uint8_t *payload, uint32_t payload_len) {
    if (!s || !RLX_LOAD(s->connected))
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
        RLX_STORE(s->connected, false);
        return -1;
    }

    /* Send payload */
    if (payload && payload_len > 0) {
        if (write_exact(s, payload, payload_len) < 0) {
            RLX_STORE(s->connected, false);
            return -1;
        }
    }

    RLX_ADD(s->bytes_sent, (uint64_t)MD_PACKET_HEADER_SIZE + payload_len);
    RLX_ADD(s->packets_sent, 1);
    return 0;
}

int md_stream_recv(MdStream *s, MdPacketHeader *hdr,
                   uint8_t **payload_out, uint32_t timeout_ms) {
    if (!s || !RLX_LOAD(s->connected) || !hdr)
        return -1;

    /* Read header */
    uint8_t hdr_buf[MD_PACKET_HEADER_SIZE];
    int ret = read_exact(s, hdr_buf, MD_PACKET_HEADER_SIZE, timeout_ms);
    if (ret != 0) {
        if (ret < 0) RLX_STORE(s->connected, false);
        return ret;  /* -1 = error, 1 = timeout */
    }

    if (md_packet_header_read(hdr, hdr_buf, sizeof(hdr_buf)) < 0) {
        RLX_STORE(s->connected, false);
        return -1;
    }

    /* Sanity check payload size */
    if (hdr->payload_len > MD_STREAM_MAX_PAYLOAD) {
        RLX_STORE(s->connected, false);
        return -1;
    }

    /* Read payload */
    uint8_t *payload = NULL;
    if (hdr->payload_len > 0) {
        payload = malloc(hdr->payload_len);
        if (!payload) {
            /* The header is already consumed but payload_len bytes remain
             * unread; we cannot resync, so tear the connection down rather
             * than let the next recv read payload as a header. */
            RLX_STORE(s->connected, false);
            return -1;
        }

        ret = read_exact(s, payload, hdr->payload_len, timeout_ms);
        if (ret != 0) {
            free(payload);
            if (ret < 0) RLX_STORE(s->connected, false);
            return ret;
        }
    }

    if (payload_out)
        *payload_out = payload;
    else
        free(payload);

    RLX_ADD(s->bytes_recv, (uint64_t)MD_PACKET_HEADER_SIZE + hdr->payload_len);
    RLX_ADD(s->packets_recv, 1);
    return 0;
}

/* Session binding deadlines share md_session_prepare_binding()'s clock. */
static uint64_t stream_now_ms64(void) {
    return (uint64_t)md_now_ms();
}

void md_stream_close(MdStream *s) {
    if (!s) return;
    s->session_bound = false;
    s->granted_caps = 0;
    RLX_STORE(s->connected, false);
    if (s->fd >= 0) shutdown(s->fd, SHUT_RDWR);
}

bool md_stream_has_capability(const MdStream *s, uint32_t caps) {
    return s && RLX_LOAD(s->connected) && s->session_bound &&
        (s->granted_caps & caps) == caps;
}

int md_stream_accept_binding(MdStream *s, MdSession *session) {
    if (!s || !RLX_LOAD(s->connected) || s->session_bound || !session ||
        session->state != MD_SESSION_NEGOTIATING || !session->stream_token[0])
        goto reject;
    uint64_t deadline = stream_now_ms64() + MD_SESSION_BIND_TIMEOUT_MS;
    uint8_t header[MD_PACKET_HEADER_SIZE];
    MdPacketHeader hdr;
    /* Never allocate the general 16 MiB packet budget for an unproven peer. */
    if (read_exact(s, header, sizeof(header), MD_SESSION_BIND_TIMEOUT_MS) != 0 ||
        md_packet_header_read(&hdr, header, sizeof(header)) < 0 ||
        hdr.type != MD_PKT_SESSION_INFO || hdr.payload_len == 0 ||
        hdr.payload_len > MD_SESSION_JSON_MAX)
        goto reject;
    uint64_t now = stream_now_ms64();
    if (now >= deadline) goto reject;
    uint8_t payload[MD_SESSION_JSON_MAX];
    if (read_exact(s, payload, hdr.payload_len, (uint32_t)(deadline - now)) != 0)
        goto reject;
    now = stream_now_ms64();
    int ret = now >= deadline ? -1 :
        md_session_consume_binding(session, payload, hdr.payload_len, now);
    OPENSSL_cleanse(payload, hdr.payload_len);
    if (ret != 0) goto reject;
    /* No token in the ACK, and no client-supplied grants are echoed. */
    MdSessionAccept ack = {
        .granted = session->capabilities,
        .tree_format = session->tree_format,
    };
    memcpy(ack.session_id, session->session_id, sizeof(ack.session_id));
    char *json = md_session_accept_to_json(&ack);
    if (!json) goto reject;
    ret = md_stream_send(s, MD_PKT_SESSION_INFO, 0,
                         (const uint8_t *)json, (uint32_t)strlen(json));
    free(json);
    if (ret != 0) goto reject;
    s->granted_caps = session->capabilities;
    s->session_bound = true;
    return 0;
reject:
    md_stream_close(s);
    return -1;
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
    stats->bytes_sent   = RLX_LOAD(s->bytes_sent);
    stats->bytes_recv   = RLX_LOAD(s->bytes_recv);
    stats->packets_sent = RLX_LOAD(s->packets_sent);
    stats->packets_recv = RLX_LOAD(s->packets_recv);
}

int md_stream_get_fd(const MdStream *s) {
    return s ? s->fd : -1;
}

bool md_stream_is_connected(const MdStream *s) {
    return s ? RLX_LOAD(s->connected) : false;
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
    pthread_mutex_destroy(&s->ssl_mu);
    free(s);
}
