/*
 * metadesk — mcp_http.c
 * Minimal HTTP/1.1 + SSE transport for MCP server.
 *
 * Handles two endpoints:
 *   POST /mcp — receives JSON-RPC, returns JSON-RPC response
 *   GET  /mcp — establishes SSE stream for notifications
 *
 * Concurrent: each accepted connection is handled by a go()
 * goroutine so POST processing doesn't block the accept loop.
 * Thread-local storage routes responses to the correct request.
 * SSE client capacity is configurable.
 */
#include "mcp_http.h"
#define MD_LOG_TAG "mcp-http"
#include "log.h"
#include <fcntl.h>
#include <poll.h>
#include "io_util.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <go.h>
#include <stdatomic.h>

#define DEFAULT_MAX_SSE_CLIENTS 4
/* 1 MB max request body — the protocol-level limit shared with stdio */
#define MAX_REQUEST_SIZE        MD_MCP_MAX_MESSAGE_SIZE
#define READ_BUF_SIZE     4096
/* Bound how long a connection handler can block on reads — without it
 * an idle client parks a handler goroutine forever and wedges destroy. */
#define CLIENT_READ_TIMEOUT_SEC 30

/* ── Server struct ───────────────────────────────────────────── */

typedef struct {
    int fd;
    bool ready; /* false while the handler owns the header write */
    size_t sent;
    bool failed;
} SseClient;

struct MdMcpHttp {
    MdMcpServer    *mcp_server;
    int             listen_fd;
    uint16_t        port;
    atomic_bool     shutdown;

    /* SSE client file descriptors */
    SseClient      *sse_clients;
    int             sse_count;
    int             sse_max_clients;
    int             sse_write_timeout_ms;
    int             sse_sndbuf;
    pthread_mutex_t sse_mu;

    /* Tracks in-flight handler goroutines for graceful shutdown */
    GoWaitGroup     handler_wg;
};

/* ── Per-request response capture ─────────────────────────────
 * A POST's response is captured through a per-request sink passed to
 * md_mcp_server_handle_message_with_sink(). A request may write more
 * than once (a response plus an immediate notification), so capture
 * all writes and return a JSON array when multiple are emitted. */
typedef struct {
    char  **items;
    size_t  count;
    size_t  cap;
    bool    failed;
} HttpResponseCapture;

static int capture_append(HttpResponseCapture *cap, const char *json, size_t len)
{
    if (!cap)
        return 0;

    if (cap->count == cap->cap) {
        size_t new_cap = cap->cap ? cap->cap * 2 : 4;
        char **new_items = realloc(cap->items, new_cap * sizeof(*new_items));
        if (!new_items) {
            cap->failed = true;
            return -1;
        }
        cap->items = new_items;
        cap->cap = new_cap;
    }

    cap->items[cap->count] = strndup(json, len);
    if (!cap->items[cap->count]) {
        cap->failed = true;
        return -1;
    }
    cap->count++;
    return 0;
}

static char *capture_take_body(HttpResponseCapture *cap)
{
    if (!cap || cap->count == 0)
        return NULL;

    if (cap->count == 1) {
        char *body = cap->items[0];
        cap->items[0] = NULL;
        return body;
    }

    /* Multiple messages: return a JSON array (cJSON, not pointer
     * arithmetic — one off-by-one here is a heap overflow). */
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;

    for (size_t i = 0; i < cap->count; i++) {
        cJSON *item = cJSON_Parse(cap->items[i]);
        if (!item) {
            cJSON_Delete(arr);
            cap->failed = true;
            return NULL;
        }
        cJSON_AddItemToArray(arr, item);
    }

    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return body;
}

static void capture_free(HttpResponseCapture *cap)
{
    if (!cap) return;
    for (size_t i = 0; i < cap->count; i++)
        free(cap->items[i]);
    free(cap->items);
    memset(cap, 0, sizeof(*cap));
}

/* Per-request response sink used by the POST handler. */
static int http_capture_write(const char *json, size_t len, void *userdata)
{
    return capture_append((HttpResponseCapture *)userdata, json, len);
}

/* Server write_fn — the out-of-band notification channel only.
 * Request responses never pass through here (they use the per-request
 * capture sink), so everything on this channel is a server-initiated
 * notification for SSE subscribers. */
int md_mcp_http_write(const char *json, size_t len, void *userdata)
{
    MdMcpHttp *h = (MdMcpHttp *)userdata;
    if (!h || !json) return -1;

    if (md_mcp_http_send_sse(h, "message", json, len) < 0)
        return -1;
    return 0;
}

/* ── HTTP response helpers ───────────────────────────────────── */

static void send_http_response(MdMcpHttp *h, int fd, int status, const char *status_text,
                               const char *content_type,
                               const char *body, size_t body_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);

    int64_t deadline = md_now_ms() + h->sse_write_timeout_ms;
    if (hlen <= 0 || (size_t)hlen >= sizeof(header) ||
        md_fd_write_deadline(fd, header, (size_t)hlen, deadline, true, NULL) < 0)
        return;
    if (body && body_len > 0)
        md_fd_write_deadline(fd, body, body_len, deadline, true, NULL);
    /* The handler always closes the fd, including on timeout/partial reply. */
}

/* Plain-text error/short response — length computed, never hand-counted. */
static void send_http_text(MdMcpHttp *h, int fd, int status, const char *status_text,
                           const char *msg)
{
    send_http_response(h, fd, status, status_text, "text/plain",
                       msg, strlen(msg));
}

static int send_sse_headers(int fd, int64_t deadline)
{
    const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    return md_fd_write_deadline(fd, headers, strlen(headers), deadline, true, NULL);
}

/* ── Parse minimal HTTP request ──────────────────────────────── */

typedef struct {
    char method[8];          /* GET or POST */
    char path[64];
    char *body;              /* heap-allocated body for POST */
    size_t body_len;
    size_t content_length;
    char content_type[128];  /* Content-Type header */
} HttpRequest;

static bool is_json_content_type(const char *content_type)
{
    if (!content_type || !*content_type)
        return false;

    while (*content_type == ' ' || *content_type == '\t')
        content_type++;

    const char *json_type = "application/json";
    size_t json_type_len = strlen(json_type);
    if (strncasecmp(content_type, json_type, json_type_len) != 0)
        return false;

    content_type += json_type_len;
    while (*content_type == ' ' || *content_type == '\t')
        content_type++;

    return *content_type == '\0' || *content_type == ';';
}

static int parse_content_length(const char *header_start, const char *headers_end,
                                size_t *content_length)
{
    *content_length = 0;

    const char *cl = strcasestr(header_start, "Content-Length:");
    if (!cl || cl >= headers_end)
        return 0;

    cl += 15;
    while (cl < headers_end && (*cl == ' ' || *cl == '\t'))
        cl++;
    if (cl >= headers_end || *cl < '0' || *cl > '9')
        return -1;

    errno = 0;
    char *endptr = NULL;
    long val = strtol(cl, &endptr, 10);
    if (errno == ERANGE || val < 0 || !endptr)
        return -1;

    const char *p = endptr;
    while (p < headers_end && (*p == ' ' || *p == '\t'))
        p++;
    if (p < headers_end && *p != '\r' && *p != '\n')
        return -1;

    *content_length = (size_t)val;
    return 0;
}

static int parse_http_request(const char *buf, size_t buf_len, HttpRequest *req)
{
    memset(req, 0, sizeof(*req));

    /* Parse request line */
    const char *end = strstr(buf, "\r\n");
    if (!end) return -1;

    if (sscanf(buf, "%7s %63s", req->method, req->path) != 2)
        return -1;

    /* Parse headers */
    const char *header_start = end + 2;
    const char *headers_end = strstr(buf, "\r\n\r\n");
    if (!headers_end) return -1;
    const char *body_start = headers_end + 4;

    /* Find Content-Length */
    if (parse_content_length(header_start, headers_end,
                             &req->content_length) < 0)
        return -1;

    /* Find Content-Type */
    const char *ct = strcasestr(header_start, "Content-Type:");
    if (ct && ct < headers_end) {
        ct += 13;
        while (*ct == ' ' || *ct == '\t') ct++;
        const char *ct_end = strstr(ct, "\r\n");
        if (ct_end && ct_end <= headers_end) {
            while (ct_end > ct && (ct_end[-1] == ' ' || ct_end[-1] == '\t'))
                ct_end--;
            size_t ct_len = (size_t)(ct_end - ct);
            if (ct_len >= sizeof(req->content_type))
                ct_len = sizeof(req->content_type) - 1;
            memcpy(req->content_type, ct, ct_len);
        }
    }

    /* Extract body */
    size_t available = buf_len - (size_t)(body_start - buf);
    if (req->content_length > 0 && available >= req->content_length) {
        req->body = strndup(body_start, req->content_length);
        req->body_len = req->content_length;
    }

    return 0;
}

/* ── Handle a client connection ──────────────────────────────── */

static void handle_client(MdMcpHttp *h, int client_fd)
{
    /* Bound read waits so idle connections cannot park a handler */
    struct timeval rcv_timeout = { .tv_sec = CLIENT_READ_TIMEOUT_SEC,
                                   .tv_usec = 0 };
    struct timeval snd_timeout = { .tv_sec = h->sse_write_timeout_ms / 1000,
        .tv_usec = (h->sse_write_timeout_ms % 1000) * 1000 };
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                   &rcv_timeout, sizeof(rcv_timeout)) < 0 ||
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                   &snd_timeout, sizeof(snd_timeout)) < 0) {
        close(client_fd);
        return;
    }

    /* Heap-allocate the request buffer to handle larger payloads */
    size_t buf_cap = READ_BUF_SIZE * 4;
    char *buf = malloc(buf_cap);
    if (!buf) {
        close(client_fd);
        return;
    }
    size_t total = 0;

    /* Read the full request — headers + body */
    while (total < buf_cap - 1) {
        ssize_t n = read(client_fd, buf + total, buf_cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';

        /* Check if we have complete headers */
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            /* Check Content-Length and enforce limit */
            size_t content_len = 0;
            if (parse_content_length(buf, body_start, &content_len) < 0) {
                send_http_text(h, client_fd, 400, "Bad Request",
                               "Invalid Content-Length\n");
                free(buf);
                close(client_fd);
                return;
            }

            /* Reject oversized requests */
            if (content_len > MAX_REQUEST_SIZE) {
                send_http_text(h, client_fd, 413, "Content Too Large",
                               "Request body too large\n");
                free(buf);
                close(client_fd);
                return;
            }

            /* Grow buffer if needed for the body */
            size_t headers_end = (size_t)(body_start + 4 - buf);
            size_t needed = headers_end + content_len + 1;
            if (needed > buf_cap) {
                buf_cap = needed;
                char *new_buf = realloc(buf, buf_cap);
                if (!new_buf) {
                    free(buf);
                    close(client_fd);
                    return;
                }
                buf = new_buf;
            }

            if (total >= headers_end + content_len)
                break;  /* Have full request */
        }
    }

    HttpRequest req;
    if (parse_http_request(buf, total, &req) != 0) {
        send_http_text(h, client_fd, 400, "Bad Request", "Bad request\n");
        free(buf);
        close(client_fd);
        return;
    }
    free(buf);

    /* All POSTs must carry JSON-RPC as application/json. */
    if (strcmp(req.method, "POST") == 0 &&
        !is_json_content_type(req.content_type)) {
        send_http_text(h, client_fd, 415, "Unsupported Media Type",
                       "Unsupported media type\n");
        free(req.body);
        close(client_fd);
        return;
    }

    /* Route by method + path */
    if (strcmp(req.path, "/mcp") == 0 && strcmp(req.method, "POST") == 0) {
        /* JSON-RPC request */
        if (!req.body || req.body_len == 0) {
            send_http_text(h, client_fd, 400, "Bad Request", "Empty body\n");
        } else {
            /* Dispatch to MCP server with a per-request capture sink —
             * this request's response(s) come back through it. */
            HttpResponseCapture capture = {0};
            md_mcp_server_handle_message_with_sink(h->mcp_server,
                                                   req.body, req.body_len,
                                                   http_capture_write, &capture);

            char *resp = capture_take_body(&capture);
            bool capture_failed = capture.failed;
            capture_free(&capture);

            if (capture_failed) {
                send_http_text(h, client_fd, 500, "Internal Server Error",
                               "Response capture failed\n");
            } else if (resp) {
                send_http_response(h, client_fd, 200, "OK",
                                   "application/json", resp, strlen(resp));
                free(resp);
            } else {
                send_http_text(h, client_fd, 202, "Accepted", "");
            }
        }
        free(req.body);
        close(client_fd);

    } else if (strcmp(req.path, "/mcp") == 0 && strcmp(req.method, "GET") == 0) {
        pthread_mutex_lock(&h->sse_mu);
        if (h->sse_count >= h->sse_max_clients || atomic_load(&h->shutdown)) {
            pthread_mutex_unlock(&h->sse_mu);
            send_http_text(h, client_fd, 503, "Service Unavailable", "SSE capacity exhausted\n");
            free(req.body);
            close(client_fd);
            return;
        }
        /* Reserve capacity, but broadcasts skip this fd until headers finish.
         * The handler owns it while unready; shutdown must not close/reuse it. */
        h->sse_clients[h->sse_count++] = (SseClient){ .fd = client_fd };
        pthread_mutex_unlock(&h->sse_mu);

        int flags = fcntl(client_fd, F_GETFL);
        bool ready = flags >= 0 && fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
            send_sse_headers(client_fd, md_now_ms() + h->sse_write_timeout_ms) == 0;
        /* 0 = never touched; verified by inspection, kernel autotuning makes a behavioural check non-deterministic. */
        if (ready && h->sse_sndbuf > 0)
            ready = setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF,
                               &h->sse_sndbuf, sizeof(h->sse_sndbuf)) == 0;
        pthread_mutex_lock(&h->sse_mu);
        for (int i = 0; i < h->sse_count; i++) {
            if (h->sse_clients[i].fd != client_fd) continue;
            if (ready && !atomic_load(&h->shutdown)) {
                h->sse_clients[i].ready = true;
            } else {
                close(client_fd);
                h->sse_clients[i] = h->sse_clients[--h->sse_count];
            }
            break;
        }
        pthread_mutex_unlock(&h->sse_mu);
        free(req.body);

    } else {
        send_http_text(h, client_fd, 404, "Not Found", "Not found\n");
        free(req.body);
        close(client_fd);
    }
}

/* ── Handler goroutine ───────────────────────────────────────── */

typedef struct {
    MdMcpHttp   *http;
    int          client_fd;
    GoWaitGroup *wg;
} HandlerArg;

static void *handle_client_thread(void *arg) {
    HandlerArg *ha = arg;
    handle_client(ha->http, ha->client_fd);
    go_wait_group_done(ha->wg);
    free(ha);
    return NULL;
}

/* ── SSE send ────────────────────────────────────────────────── */

/* Encode once so each client can advance independently within one budget. */
static char *make_sse_frame(const char *event, const char *data, size_t len,
                             size_t *frame_len)
{
    char header[128];
    int hlen = event ? snprintf(header, sizeof(header), "event: %s\n", event) : 0;
    if (hlen < 0 || (size_t)hlen >= sizeof(header) || len > (SIZE_MAX - 136) / 8)
        return NULL;
    size_t lines = 1;
    for (size_t i = 0; i + 1 < len; i++)
        if (data[i] == '\n') lines++;
    char *frame = malloc((size_t)hlen + len + lines * 7 + 1);
    if (!frame) return NULL;
    char *out = frame;
    memcpy(out, header, (size_t)hlen);
    out += hlen;
    size_t pos = 0;
    do {
        const char *line = data + pos;
        const char *nl = memchr(line, '\n', len - pos);
        size_t n = nl ? (size_t)(nl - line) : len - pos;
        if (n > 0 && line[n - 1] == '\r') n--;
        memcpy(out, "data: ", 6); out += 6;
        memcpy(out, line, n); out += n;
        *out++ = '\n';
        if (!nl) break;
        pos = (size_t)(nl - data) + 1;
    } while (pos < len);
    *out++ = '\n';
    *frame_len = (size_t)(out - frame);
    return frame;
}

int md_mcp_http_send_sse(MdMcpHttp *http, const char *event,
                         const char *data, size_t data_len)
{
    if (!http || !data) return -1;

    size_t frame_len;
    char *frame = make_sse_frame(event, data, data_len, &frame_len);
    if (!frame) return -1;
    pthread_mutex_lock(&http->sse_mu);
    int64_t deadline = md_now_ms() + http->sse_write_timeout_ms;
    struct pollfd *fds = calloc((size_t)http->sse_max_clients, sizeof(*fds));
    if (!fds) {
        pthread_mutex_unlock(&http->sse_mu);
        free(frame);
        return -1;
    }
    int pending = 0;
    for (int i = 0; i < http->sse_count; i++) {
        SseClient *c = &http->sse_clients[i];
        c->sent = 0;
        c->failed = false;
        fds[i] = (struct pollfd){ .fd = c->ready ? c->fd : -1, .events = POLLOUT };
        if (c->ready) pending++;
    }
    while (pending > 0) {
        int64_t remaining = deadline - md_now_ms();
        if (remaining <= 0) break;
        int rc = poll(fds, (nfds_t)http->sse_count, (int)remaining);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) break;
        for (int i = 0; i < http->sse_count; i++) {
            if (fds[i].fd < 0 || !fds[i].revents) continue;
            SseClient *c = &http->sse_clients[i];
            ssize_t n = md_socket_write(c->fd, frame + c->sent, frame_len - c->sent);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (n <= 0) c->failed = true;
            else c->sent += (size_t)n;
            if (c->failed || c->sent == frame_len) {
                fds[i].fd = -1;
                pending--;
            }
        }
    }
    for (int i = 0; i < http->sse_count; ) {
        SseClient *c = &http->sse_clients[i];
        if (c->ready && (c->failed || c->sent != frame_len)) {
            MD_LOG_W("dropping SSE client after failed or timed-out write");
            close(c->fd);
            *c = http->sse_clients[--http->sse_count];
        } else {
            i++;
        }
    }
    free(fds);
    pthread_mutex_unlock(&http->sse_mu);
    free(frame);
    return 0;
}

int md_mcp_http_sse_client_count(MdMcpHttp *http)
{
    if (!http) return 0;
    pthread_mutex_lock(&http->sse_mu);
    int count = 0;
    for (int i = 0; i < http->sse_count; i++)
        if (http->sse_clients[i].ready) count++;
    pthread_mutex_unlock(&http->sse_mu);
    return count;
}

int md_mcp_http_sse_client_sndbuf(MdMcpHttp *http, int index)
{
    if (!http || index < 0) return -1;
    int sndbuf = -1;
    pthread_mutex_lock(&http->sse_mu);
    for (int i = 0; i < http->sse_count; i++) {
        if (!http->sse_clients[i].ready || index-- != 0) continue;
        socklen_t len = sizeof(sndbuf);
        if (getsockopt(http->sse_clients[i].fd, SOL_SOCKET, SO_SNDBUF,
                       &sndbuf, &len) < 0)
            sndbuf = -1;
        break;
    }
    pthread_mutex_unlock(&http->sse_mu);
    return sndbuf;
}

static void close_sse_clients(MdMcpHttp *http)
{
    if (!http) return;

    pthread_mutex_lock(&http->sse_mu);
    for (int i = 0; i < http->sse_count; ) {
        if (http->sse_clients[i].ready) {
            close(http->sse_clients[i].fd);
            http->sse_clients[i] = http->sse_clients[--http->sse_count];
        } else {
            i++; /* header writer owns this fd and observes shutdown */
        }
    }
    pthread_mutex_unlock(&http->sse_mu);
}

/* ── Lifecycle ───────────────────────────────────────────────── */

MdMcpHttp *md_mcp_http_create(const MdMcpHttpConfig *config)
{
    if (!config || !config->server) return NULL;

    MdMcpHttp *h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->mcp_server = config->server;
    h->port = config->port > 0 ? config->port : MD_MCP_HTTP_DEFAULT_PORT;
    atomic_init(&h->shutdown, false);
    h->sse_max_clients = config->max_clients > 0
        ? config->max_clients : DEFAULT_MAX_SSE_CLIENTS;
    h->sse_write_timeout_ms = config->sse_write_timeout_ms > 0
        ? config->sse_write_timeout_ms : MD_MCP_SSE_WRITE_TIMEOUT_MS;
    h->sse_sndbuf = config->sse_sndbuf;
    h->sse_clients = calloc((size_t)h->sse_max_clients,
                            sizeof(*h->sse_clients));
    if (!h->sse_clients) {
        free(h);
        return NULL;
    }
    pthread_mutex_init(&h->sse_mu, NULL);
    go_wait_group_init(&h->handler_wg);

    /* Create listening socket */
    h->listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (h->listen_fd < 0) {
        go_wait_group_destroy(&h->handler_wg);
        pthread_mutex_destroy(&h->sse_mu);
        free(h->sse_clients);
        free(h);
        return NULL;
    }

    int opt = 1;
    setsockopt(h->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Dual-stack: allow IPv4 connections on IPv6 socket */
    int off = 0;
    setsockopt(h->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(h->port);

    if (config->bind_addr) {
        if (inet_pton(AF_INET6, config->bind_addr, &addr.sin6_addr) != 1) {
            struct in_addr v4_addr;
            if (inet_pton(AF_INET, config->bind_addr, &v4_addr) == 1) {
                memset(&addr.sin6_addr, 0, sizeof(addr.sin6_addr));
                addr.sin6_addr.s6_addr[10] = 0xff;
                addr.sin6_addr.s6_addr[11] = 0xff;
                memcpy(&addr.sin6_addr.s6_addr[12], &v4_addr, sizeof(v4_addr));
            } else {
                close(h->listen_fd);
                go_wait_group_destroy(&h->handler_wg);
                pthread_mutex_destroy(&h->sse_mu);
                free(h->sse_clients);
                free(h);
                return NULL;
            }
        }
    } else {
        addr.sin6_addr = in6addr_loopback;  /* localhost only by default */
    }

    if (bind(h->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(h->listen_fd);
        go_wait_group_destroy(&h->handler_wg);
        pthread_mutex_destroy(&h->sse_mu);
        free(h->sse_clients);
        free(h);
        return NULL;
    }

    if (listen(h->listen_fd, 8) < 0) {
        close(h->listen_fd);
        go_wait_group_destroy(&h->handler_wg);
        pthread_mutex_destroy(&h->sse_mu);
        free(h->sse_clients);
        free(h);
        return NULL;
    }

    return h;
}

int md_mcp_http_run(MdMcpHttp *http)
{
    if (!http) return -1;

    while (!atomic_load(&http->shutdown)) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(http->listen_fd, &readfds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int nfds = http->listen_fd + 1;

        int ret = select(nfds, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) continue;  /* timeout — check shutdown flag */

        if (FD_ISSET(http->listen_fd, &readfds)) {
            int client = accept(http->listen_fd, NULL, NULL);
            if (client >= 0) {
#ifdef SO_NOSIGPIPE
                int no_sigpipe = 1;
                if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE,
                               &no_sigpipe, sizeof(no_sigpipe)) < 0) {
                    close(client);
                    continue;
                }
#endif
                /* Spawn a goroutine per connection so POST processing
                 * doesn't block the accept loop. */
                HandlerArg *ha = malloc(sizeof(HandlerArg));
                if (ha) {
                    ha->http      = http;
                    ha->client_fd = client;
                    ha->wg        = &http->handler_wg;
                    go_wait_group_add(&http->handler_wg, 1);
                    go(handle_client_thread, ha);
                } else {
                    close(client);
                }
            }
        }
    }

    return 0;
}

void md_mcp_http_shutdown(MdMcpHttp *http)
{
    if (!http) return;
    atomic_store(&http->shutdown, true);
    close_sse_clients(http);
}

void md_mcp_http_destroy(MdMcpHttp *http)
{
    if (!http) return;

    /* Wait for in-flight handler goroutines to finish */
    go_wait_group_wait(&http->handler_wg);
    go_wait_group_destroy(&http->handler_wg);

    /* Close SSE clients */
    close_sse_clients(http);

    if (http->listen_fd >= 0)
        close(http->listen_fd);

    pthread_mutex_destroy(&http->sse_mu);
    free(http->sse_clients);
    free(http);
}
