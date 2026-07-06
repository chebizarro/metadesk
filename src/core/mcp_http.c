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

#define DEFAULT_MAX_SSE_CLIENTS 4
#define MAX_REQUEST_SIZE        (1024 * 1024)  /* 1 MB max request body */
#define READ_BUF_SIZE     4096

/* ── Server struct ───────────────────────────────────────────── */

struct MdMcpHttp {
    MdMcpServer    *mcp_server;
    int             listen_fd;
    uint16_t        port;
    volatile bool   shutdown;

    /* SSE client file descriptors */
    int            *sse_clients;
    int             sse_count;
    int             sse_max_clients;
    pthread_mutex_t sse_mu;

    /* Tracks in-flight handler goroutines for graceful shutdown */
    GoWaitGroup     handler_wg;
};

/* ── Thread-local response capture ───────────────────────────
 * handle_message() calls write_fn synchronously on the calling
 * thread, so TLS gives us per-request response isolation without
 * any shared mutable state. */
static _Thread_local char *tls_response = NULL;

static int http_write_fn(const char *json, size_t len, void *userdata)
{
    MdMcpHttp *h = (MdMcpHttp *)userdata;
    if (!h) return -1;

    /* Capture response for the calling POST handler (same thread) */
    free(tls_response);
    tls_response = strndup(json, len);

    /* Also broadcast as SSE to all subscribed clients */
    md_mcp_http_send_sse(h, "message", json, len);

    return 0;
}

/* ── HTTP response helpers ───────────────────────────────────── */

static void send_http_response(int fd, int status, const char *status_text,
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

    write(fd, header, (size_t)hlen);
    if (body && body_len > 0)
        write(fd, body, body_len);
}

static void send_sse_headers(int fd)
{
    const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    write(fd, headers, strlen(headers));
}

/* ── Parse minimal HTTP request ──────────────────────────────── */

typedef struct {
    char method[8];          /* GET or POST */
    char path[64];
    char *body;              /* heap-allocated body for POST */
    size_t body_len;
    size_t content_length;
    char content_type[128];  /* Content-Type header */
    char session_id[128];    /* Mcp-Session-Id header */
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
    const char *cl = strcasestr(header_start, "Content-Length:");
    if (cl && cl < headers_end) {
        cl += 15;
        while (*cl == ' ' || *cl == '\t') cl++;
        req->content_length = (size_t)atol(cl);
    }

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

    /* Find Mcp-Session-Id */
    const char *sid = strcasestr(header_start, "Mcp-Session-Id:");
    if (sid && sid < headers_end) {
        sid += 15;
        while (*sid == ' ' || *sid == '\t') sid++;
        const char *sid_end = strstr(sid, "\r\n");
        if (sid_end && sid_end <= headers_end) {
            size_t slen = (size_t)(sid_end - sid);
            if (slen >= sizeof(req->session_id)) slen = sizeof(req->session_id) - 1;
            memcpy(req->session_id, sid, slen);
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
            const char *cl = strcasestr(buf, "Content-Length:");
            size_t content_len = 0;
            if (cl) content_len = (size_t)atol(cl + 15);

            /* Reject oversized requests */
            if (content_len > MAX_REQUEST_SIZE) {
                send_http_response(client_fd, 413,
                                   "Content Too Large",
                                   "text/plain",
                                   "Request body too large\n", 23);
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
        send_http_response(client_fd, 400, "Bad Request",
                           "text/plain", "Bad request\n", 12);
        free(buf);
        close(client_fd);
        return;
    }
    free(buf);

    /* All POSTs must carry JSON-RPC as application/json. */
    if (strcmp(req.method, "POST") == 0 &&
        !is_json_content_type(req.content_type)) {
        send_http_response(client_fd, 415, "Unsupported Media Type",
                           "text/plain", "Unsupported media type\n", 23);
        free(req.body);
        close(client_fd);
        return;
    }

    /* Route by method + path */
    if (strcmp(req.path, "/mcp") == 0 && strcmp(req.method, "POST") == 0) {
        /* JSON-RPC request */
        if (!req.body || req.body_len == 0) {
            send_http_response(client_fd, 400, "Bad Request",
                               "text/plain", "Empty body\n", 11);
        } else {
            /* Dispatch to MCP server — write_fn stores response in TLS */
            tls_response = NULL;
            md_mcp_server_handle_message(h->mcp_server, req.body, req.body_len);

            char *resp = tls_response;
            tls_response = NULL;

            if (resp) {
                send_http_response(client_fd, 200, "OK",
                                   "application/json", resp, strlen(resp));
                free(resp);
            } else {
                send_http_response(client_fd, 202, "Accepted",
                                   "text/plain", "", 0);
            }
        }
        free(req.body);
        close(client_fd);

    } else if (strcmp(req.path, "/mcp") == 0 && strcmp(req.method, "GET") == 0) {
        /* SSE stream */
        send_sse_headers(client_fd);

        pthread_mutex_lock(&h->sse_mu);
        if (h->sse_count < h->sse_max_clients) {
            h->sse_clients[h->sse_count++] = client_fd;
        } else {
            close(client_fd);
        }
        pthread_mutex_unlock(&h->sse_mu);
        /* Don't close — kept open for SSE */
        free(req.body);

    } else {
        send_http_response(client_fd, 404, "Not Found",
                           "text/plain", "Not found\n", 10);
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

int md_mcp_http_send_sse(MdMcpHttp *http, const char *event,
                         const char *data, size_t data_len)
{
    if (!http || !data) return -1;

    pthread_mutex_lock(&http->sse_mu);

    for (int i = 0; i < http->sse_count; ) {
        /* Format: "event: <event>\ndata: <json>\n\n" */
        char header_buf[128];
        int hlen = 0;
        if (event)
            hlen = snprintf(header_buf, sizeof(header_buf),
                           "event: %s\ndata: ", event);
        else
            hlen = snprintf(header_buf, sizeof(header_buf), "data: ");

        ssize_t w1 = write(http->sse_clients[i], header_buf, (size_t)hlen);
        ssize_t w2 = write(http->sse_clients[i], data, data_len);
        ssize_t w3 = write(http->sse_clients[i], "\n\n", 2);

        if (w1 <= 0 || w2 <= 0 || w3 <= 0) {
            /* Client disconnected — remove from list */
            close(http->sse_clients[i]);
            http->sse_clients[i] = http->sse_clients[http->sse_count - 1];
            http->sse_count--;
        } else {
            i++;
        }
    }

    pthread_mutex_unlock(&http->sse_mu);
    return 0;
}

/* ── Lifecycle ───────────────────────────────────────────────── */

MdMcpHttp *md_mcp_http_create(const MdMcpHttpConfig *config)
{
    if (!config || !config->server) return NULL;

    MdMcpHttp *h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->mcp_server = config->server;
    h->port = config->port > 0 ? config->port : MD_MCP_HTTP_DEFAULT_PORT;
    h->shutdown = false;
    h->sse_max_clients = config->max_clients > 0
        ? config->max_clients : DEFAULT_MAX_SSE_CLIENTS;
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

    while (!http->shutdown) {
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
    if (http) http->shutdown = true;
}

MdMcpWriteFn md_mcp_http_get_write_fn(MdMcpHttp *http)
{
    (void)http;
    return http_write_fn;
}

void *md_mcp_http_get_write_userdata(MdMcpHttp *http)
{
    return http;
}

void md_mcp_http_destroy(MdMcpHttp *http)
{
    if (!http) return;

    /* Wait for in-flight handler goroutines to finish */
    go_wait_group_wait(&http->handler_wg);
    go_wait_group_destroy(&http->handler_wg);

    /* Close SSE clients */
    pthread_mutex_lock(&http->sse_mu);
    for (int i = 0; i < http->sse_count; i++)
        close(http->sse_clients[i]);
    http->sse_count = 0;
    pthread_mutex_unlock(&http->sse_mu);

    if (http->listen_fd >= 0)
        close(http->listen_fd);

    pthread_mutex_destroy(&http->sse_mu);
    free(http->sse_clients);
    free(http);
}
