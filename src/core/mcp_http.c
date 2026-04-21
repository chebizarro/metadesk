/*
 * metadesk — mcp_http.c
 * Minimal HTTP/1.1 + SSE transport for MCP server.
 *
 * Handles two endpoints:
 *   POST /mcp — receives JSON-RPC, returns JSON-RPC response
 *   GET  /mcp — establishes SSE stream for notifications
 *
 * Single-threaded, uses select() for multiplexing.
 * Max 4 concurrent SSE clients.
 */
#include "mcp_http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAX_SSE_CLIENTS   4
#define MAX_REQUEST_SIZE  (1024 * 1024)  /* 1 MB max request body */
#define READ_BUF_SIZE     4096

/* ── Server struct ───────────────────────────────────────────── */

struct MdMcpHttp {
    MdMcpServer    *mcp_server;
    int             listen_fd;
    uint16_t        port;
    volatile bool   shutdown;

    /* SSE client file descriptors */
    int             sse_clients[MAX_SSE_CLIENTS];
    int             sse_count;
    pthread_mutex_t sse_mu;

    /* For POST responses: the current response is captured here */
    pthread_mutex_t response_mu;
    char           *pending_response;
};

/* ── Write callback ──────────────────────────────────────────── */

static int http_write_fn(const char *json, size_t len, void *userdata)
{
    MdMcpHttp *h = (MdMcpHttp *)userdata;
    if (!h) return -1;

    /* Store the response for the POST handler to pick up */
    pthread_mutex_lock(&h->response_mu);
    free(h->pending_response);
    h->pending_response = strndup(json, len);
    pthread_mutex_unlock(&h->response_mu);

    /* Also send as SSE to all subscribed clients */
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
        "Access-Control-Allow-Origin: *\r\n"
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
        "Access-Control-Allow-Origin: *\r\n"
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
    char session_id[128];    /* Mcp-Session-Id header */
} HttpRequest;

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
    const char *body_start = strstr(buf, "\r\n\r\n");
    if (!body_start) return -1;
    body_start += 4;

    /* Find Content-Length */
    const char *cl = strcasestr(header_start, "Content-Length:");
    if (cl) {
        cl += 15;
        while (*cl == ' ') cl++;
        req->content_length = (size_t)atol(cl);
    }

    /* Find Mcp-Session-Id */
    const char *sid = strcasestr(header_start, "Mcp-Session-Id:");
    if (sid) {
        sid += 15;
        while (*sid == ' ') sid++;
        const char *sid_end = strstr(sid, "\r\n");
        if (sid_end) {
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
    char buf[READ_BUF_SIZE * 4];
    size_t total = 0;

    /* Read the full request (simple: read until we have headers + body) */
    while (total < sizeof(buf) - 1) {
        ssize_t n = read(client_fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';

        /* Check if we have complete headers */
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            /* Check if we have the full body */
            const char *cl = strcasestr(buf, "Content-Length:");
            size_t content_len = 0;
            if (cl) content_len = (size_t)atol(cl + 15);

            size_t headers_end = (size_t)(body_start + 4 - buf);
            if (total >= headers_end + content_len)
                break;  /* Have full request */
        }
    }

    HttpRequest req;
    if (parse_http_request(buf, total, &req) != 0) {
        send_http_response(client_fd, 400, "Bad Request",
                           "text/plain", "Bad request\n", 12);
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
            /* Clear pending response */
            pthread_mutex_lock(&h->response_mu);
            free(h->pending_response);
            h->pending_response = NULL;
            pthread_mutex_unlock(&h->response_mu);

            /* Dispatch to MCP server */
            md_mcp_server_handle_message(h->mcp_server, req.body, req.body_len);

            /* Retrieve response */
            pthread_mutex_lock(&h->response_mu);
            char *resp = h->pending_response;
            h->pending_response = NULL;
            pthread_mutex_unlock(&h->response_mu);

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
        if (h->sse_count < MAX_SSE_CLIENTS) {
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
    pthread_mutex_init(&h->sse_mu, NULL);
    pthread_mutex_init(&h->response_mu, NULL);

    /* Create listening socket */
    h->listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (h->listen_fd < 0) {
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
        inet_pton(AF_INET6, config->bind_addr, &addr.sin6_addr);
    } else {
        addr.sin6_addr = in6addr_loopback;  /* localhost only by default */
    }

    if (bind(h->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(h->listen_fd);
        free(h);
        return NULL;
    }

    if (listen(h->listen_fd, 8) < 0) {
        close(h->listen_fd);
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
                handle_client(http, client);
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

    /* Close SSE clients */
    pthread_mutex_lock(&http->sse_mu);
    for (int i = 0; i < http->sse_count; i++)
        close(http->sse_clients[i]);
    http->sse_count = 0;
    pthread_mutex_unlock(&http->sse_mu);

    if (http->listen_fd >= 0)
        close(http->listen_fd);

    pthread_mutex_destroy(&http->sse_mu);
    pthread_mutex_destroy(&http->response_mu);
    free(http->pending_response);
    free(http);
}
