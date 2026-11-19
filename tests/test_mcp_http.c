/*
 * metadesk — tests/test_mcp_http.c
 * Unit tests for the HTTP+SSE MCP transport.
 *
 * Tests create/destroy lifecycle, HTTP request handling via raw
 * TCP client, and shutdown behavior.
 */
#include "mcp_server.h"
#include "md_mcp_fixture.h"
#include "mcp_http.h"
#include "jsonrpc.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip98/nip98.h>
#include <nostr/nip19/nip19.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include "md_test_net.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "md_test.h"


/* Creation failures are prerequisites, not permission to run a case against
 * an unrelated listener or cascade through uninitialized response buffers. */
#define REQUIRE_HTTP(http, mcp) do { \
    MD_CHECK((http) != NULL); \
    if (!(http)) { md_mcp_server_destroy(mcp); return; } \
} while (0)

/* ── Helper: create a minimal MCP server ─────────────────────── */

static int dummy_write(const char *json, size_t len, void *userdata)
{
    (void)json; (void)len; (void)userdata;
    return 0;
}

static MdMcpServer *make_mcp_server(void)
{
    MdMcpServerConfig cfg = {
        .server_name    = "test-http",
        .server_version = "0.0.1",
        .write_fn       = dummy_write,
    };
    return md_mcp_server_create(&cfg);
}

static void wire_server_to_http(MdMcpServer *mcp, MdMcpHttp *http)
{
    MD_CHECK(md_mcp_server_set_write_fn(mcp, md_mcp_http_write, http) == 0);
}

/* ── Helper: raw TCP connect to localhost ─────────────────────── */

static int tcp_connect_opts(uint16_t port, int rcvbuf)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    /* Set the receive window before TCP negotiates window scaling. */
    if (rcvbuf > 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(port);
    addr.sin6_addr   = in6addr_loopback;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    /* macOS may enlarge SO_RCVBUF during connect. Keep the negotiated small
     * window scaling, then restore the requested limit before sending HTTP. */
    if (rcvbuf > 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int tcp_connect(uint16_t port)
{
    return tcp_connect_opts(port, 0);
}

static void shutdown_http(MdMcpHttp *http)
{
    md_mcp_http_shutdown(http);
    /* Wake the accept loop's one-second select after setting shutdown. This
     * is a test-owned connection, not a change to production shutdown policy. */
    int fd = tcp_connect(md_mcp_http_port(http));
    MD_CHECK(fd >= 0);
    if (fd >= 0) close(fd);
}

/* ── Helper: send HTTP request, read response ────────────────── */

static int http_request(uint16_t port, const char *request,
                        char *resp_buf, size_t resp_buf_len)
{
    int fd = tcp_connect(port);
    if (fd < 0) return -1;

    ssize_t w = write(fd, request, strlen(request));
    if (w <= 0) { close(fd); return -1; }

    /* Read response with timeout. The server writes headers and body in
     * separate write() calls and then closes (Connection: close), so read
     * until EOF rather than trusting a single read() to see both. */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, resp_buf + total, resp_buf_len - 1 - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= resp_buf_len - 1) break;
    }
    close(fd);

    if (total == 0) return -1;
    resp_buf[total] = '\0';
    return (int)total;
}

static int sse_connect_opts(uint16_t port, int rcvbuf, char *headers_buf,
                            size_t headers_buf_len)
{
    int fd = tcp_connect_opts(port, rcvbuf);
    if (fd < 0) return -1;

    const char *request = "GET /mcp HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ssize_t w = write(fd, request, strlen(request));
    if (w <= 0) { close(fd); return -1; }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t n = read(fd, headers_buf, headers_buf_len - 1);
    if (n <= 0) { close(fd); return -1; }
    headers_buf[n] = '\0';
    return fd;
}

static int sse_connect(uint16_t port, char *headers_buf,
                       size_t headers_buf_len)
{
    return sse_connect_opts(port, 0, headers_buf, headers_buf_len);
}

/* Headers may arrive before ready=true. Wait for publication before sending
 * exactly one probe; resending on a read timeout can leave duplicate frames. */
static void wait_sse_ready(MdMcpHttp *http, int fd, int expected_clients)
{
    int64_t deadline = md_now_ms() + 5000;
    int clients;
    while ((clients = md_mcp_http_sse_client_count(http)) != expected_clients &&
           md_now_ms() < deadline)
        sched_yield();
    MD_CHECK(clients == expected_clients);
    if (clients != expected_clients) return;

    MD_CHECK(md_mcp_http_send_sse(http, "ready", "", 0) == 0);
    const char expected[] = "event: ready\ndata: \n\n";
    char buf[sizeof(expected)];
    size_t total = 0;
    while (total < sizeof(expected) - 1) {
        int64_t remaining = deadline - md_now_ms();
        if (remaining <= 0 || md_test_wait_readable(fd, (int)remaining) != 1) break;
        ssize_t n = read(fd, buf + total, sizeof(expected) - 1 - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        total += (size_t)n;
    }
    MD_CHECK(total == sizeof(expected) - 1 && memcmp(buf, expected, total) == 0);
}

/* ── Test: NULL config ───────────────────────────────────────── */

static void test_create_null(void)
{
    MD_CHECK(md_mcp_http_create(NULL) == NULL);
    MD_CHECK(md_mcp_http_sse_client_count(NULL) == 0);
    MD_CHECK(md_mcp_http_port(NULL) == 0);

    MdMcpHttpConfig cfg = { .server = NULL };
    MD_CHECK(md_mcp_http_create(&cfg) == NULL);

    PASS("create null config");
}

static void test_create_invalid_bind_addr(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .bind_addr = "not-an-ip-address",
        .ephemeral_port = true,
    };
    MD_CHECK(md_mcp_http_create(&cfg) == NULL);

    md_mcp_server_destroy(mcp);
    PASS("invalid bind address fails");
}

/* ── Test: create and destroy ────────────────────────────────── */

static void test_create_destroy(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    MD_CHECK(md_mcp_http_port(http) != 0);
    MdMcpHttp *other = md_mcp_http_create(&cfg);
    MD_CHECK(other != NULL);
    if (!other) {
        md_mcp_http_destroy(http);
        md_mcp_server_destroy(mcp);
        return;
    }
    MD_CHECK(md_mcp_http_port(other) != 0);
    MD_CHECK(md_mcp_http_port(other) != md_mcp_http_port(http));
    /* A live listener owns its port for the entire case: no reserve/close
     * race. Explicit ports still fail rather than silently relocating. */
    cfg.ephemeral_port = false;
    cfg.port = md_mcp_http_port(http);
    MdMcpHttp *conflict = md_mcp_http_create(&cfg);
    MD_CHECK(conflict == NULL);
    md_mcp_http_destroy(conflict);
    md_mcp_http_destroy(other);

    /* The transport exposes its notification sink as a plain
     * MdMcpWriteFn taking the MdMcpHttp* as userdata. */
    MdMcpWriteFn fn = md_mcp_http_write;
    MD_CHECK(fn != NULL);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("create and destroy");
}

/* ── Test: destroy NULL is safe ──────────────────────────────── */

static void test_destroy_null(void)
{
    md_mcp_http_destroy(NULL); /* should not crash */
    PASS("destroy null");
}

/* ── Background server thread ────────────────────────────────── */

typedef struct {
    MdMcpHttp *http;
    int        result;
} ServerArgs;

static void *server_thread(void *arg)
{
    ServerArgs *sa = arg;
    sa->result = md_mcp_http_run(sa->http);
    return NULL;
}

/* ── Test: POST /mcp → JSON-RPC initialize ───────────────────── */

static void test_post_initialize(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);
    wire_server_to_http(mcp, http);

    /* Start server in background */
    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);

    const char *body = "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
                       "\"id\":1,\"params\":{\"protocolVersion\":\"" MD_TEST_MCP_PROTOCOL "\","
                       "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}";

    char request[2048];
    snprintf(request, sizeof(request),
             "POST /mcp HTTP/1.1\r\n"
             "Host: localhost:%d\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n%s",
             port, strlen(body), body);

    char response[4096];
    int n = http_request(port, request, response, sizeof(response));
    MD_CHECK(n > 0);
    MD_CHECK(strstr(response, "HTTP/1.1 200 OK") != NULL);
    MD_CHECK(strstr(response, "Access-Control-Allow-Origin") == NULL);
    MD_CHECK(strstr(response, "\"result\"") != NULL);
    MD_CHECK(strstr(response, "\"protocolVersion\":\"" MD_TEST_MCP_PROTOCOL "\"") != NULL);

    /* Shutdown */
    shutdown_http(http);
    pthread_join(tid, NULL);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("POST /mcp HTTP routing");
}

static void test_invalid_content_length_rejected(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);

    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);

    const char *request =
        "POST /mcp HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: abc\r\n"
        "\r\n{}";

    char response[2048];
    int n = http_request(port, request, response, sizeof(response));
    MD_CHECK(n > 0);
    MD_CHECK(strstr(response, "400") != NULL);

    request =
        "POST /mcp HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: -1\r\n"
        "\r\n{}";
    n = http_request(port, request, response, sizeof(response));
    MD_CHECK(n > 0);
    MD_CHECK(strstr(response, "400") != NULL);

    shutdown_http(http);
    pthread_join(tid, NULL);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("invalid Content-Length rejected");
}

static void test_post_requires_json_content_type(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);

    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);

    const char *body = "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"id\":1}";
    char request[1024];
    snprintf(request, sizeof(request),
             "POST /mcp HTTP/1.1\r\n"
             "Host: localhost:%d\r\n"
             "Content-Length: %zu\r\n"
             "\r\n%s",
             port, strlen(body), body);

    char response[2048];
    int n = http_request(port, request, response, sizeof(response));
    MD_CHECK(n > 0);
    MD_CHECK(strstr(response, "415") != NULL);
    MD_CHECK(strstr(response, "Access-Control-Allow-Origin") == NULL);

    snprintf(request, sizeof(request),
             "POST /mcp HTTP/1.1\r\n"
             "Host: localhost:%d\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Length: %zu\r\n"
             "\r\n%s",
             port, strlen(body), body);
    n = http_request(port, request, response, sizeof(response));
    MD_CHECK(n > 0);
    MD_CHECK(strstr(response, "415") != NULL);

    shutdown_http(http);
    pthread_join(tid, NULL);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("POST /mcp requires application/json Content-Type");
}

/* ── Test: GET unknown path → 404 ────────────────────────────── */

static void test_get_404(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);

    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);

    const char *request = "GET /unknown HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char response[2048];
    int n = http_request(port, request, response, sizeof(response));
    MD_CHECK(n > 0);
    MD_CHECK(strstr(response, "404") != NULL);
    MD_CHECK(strstr(response, "Access-Control-Allow-Origin") == NULL);

    shutdown_http(http);
    pthread_join(tid, NULL);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("GET unknown path → 404");
}

static void test_sse_multiline_data(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);

    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);

    char headers[1024];
    int fd = sse_connect(port, headers, sizeof(headers));
    MD_CHECK(fd >= 0);
    MD_CHECK(strstr(headers, "text/event-stream") != NULL);

    wait_sse_ready(http, fd, 1);
    MD_CHECK(md_mcp_http_send_sse(http, "multi", "line1\nline2", 11) == 0);

    char event[1024];
    ssize_t n = read(fd, event, sizeof(event) - 1);
    MD_CHECK(n > 0);
    size_t total = (size_t)n;
    event[total] = '\0';
    for (int i = 0; i < 3 && strstr(event, "data: line2\n") == NULL; i++) {
        n = read(fd, event + total, sizeof(event) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        event[total] = '\0';
    }
    MD_CHECK(strstr(event, "event: multi\n") != NULL);
    MD_CHECK(strstr(event, "data: line1\n") != NULL);
    MD_CHECK(strstr(event, "data: line2\n") != NULL);
    MD_CHECK(strstr(event, "data: line1\nline2") == NULL);

    close(fd);
    shutdown_http(http);
    pthread_join(tid, NULL);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("SSE multiline data formatting");
}

static void test_sse_max_clients_config(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
        .max_clients = 1,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);

    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);

    char headers1[1024];
    int fd1 = sse_connect(port, headers1, sizeof(headers1));
    MD_CHECK(fd1 >= 0);
    MD_CHECK(strstr(headers1, "text/event-stream") != NULL);
    MD_CHECK(strstr(headers1, "Access-Control-Allow-Origin") == NULL);

    char headers2[1024];
    int fd2 = sse_connect(port, headers2, sizeof(headers2));
    MD_CHECK(fd2 >= 0);
    MD_CHECK(strstr(headers2, "503 Service Unavailable") != NULL);
    MD_CHECK(strstr(headers2, "text/event-stream") == NULL);

    /* fd1's registration races with send_sse (headers are sent before
     * the client is added to the SSE list) — retry until it arrives. */
    char event[1024];
    size_t total = 0;
    event[0] = '\0';
    for (int i = 0; i < 40 && strstr(event, "data: data") == NULL; i++) {
        MD_CHECK(md_mcp_http_send_sse(http, "test", "data", 4) == 0);
        if (md_test_wait_readable(fd1, 100) != 1)
            continue;
        ssize_t n = read(fd1, event + total, sizeof(event) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        event[total] = '\0';
    }
    MD_CHECK(strstr(event, "event: test") != NULL);
    MD_CHECK(strstr(event, "data: data") != NULL);

    ssize_t n = read(fd2, event, sizeof(event) - 1);
    if (n > 0) {
        event[n] = '\0';
        MD_CHECK(strstr(event, "event: test") == NULL);
    } else {
        MD_CHECK(n == 0 || errno == EAGAIN || errno == EWOULDBLOCK);
    }

    close(fd1);
    close(fd2);
    shutdown_http(http);
    pthread_join(tid, NULL);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("SSE max_clients config");
}

typedef struct {
    int fd;
    const char *frame;
    size_t frame_len;
    size_t count; /* zero: read until the done sentinel */
    size_t received;
    bool matched;
} SseReader;

static void *read_sse_frames(void *arg)
{
    SseReader *r = arg;
    const char done[] = "event: done\ndata: \n\n";
    size_t pos = 0;
    bool message_matches = true, done_matches = r->count == 0;
    char buf[16384];
    for (;;) {
        if (md_test_wait_readable(r->fd, 5000) != 1) return NULL;
        ssize_t n = read(r->fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return NULL;
        for (ssize_t i = 0; i < n; i++) {
            message_matches = message_matches && pos < r->frame_len &&
                              buf[i] == r->frame[pos];
            done_matches = done_matches && pos < sizeof(done) - 1 &&
                           buf[i] == done[pos];
            if (!message_matches && !done_matches) return NULL;
            pos++;
            if (done_matches && pos == sizeof(done) - 1) {
                r->matched = i + 1 == n;
                return NULL;
            }
            if (message_matches && pos == r->frame_len) {
                r->received++;
                if (r->count && r->received == r->count) {
                    r->matched = i + 1 == n;
                    return NULL;
                }
                pos = 0;
                message_matches = true;
                done_matches = r->count == 0;
            }
        }
    }
}

static void test_sse_stalled_client(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);
    md_test_mcp_handshake(mcp);
    MdMcpHttpConfig cfg = {
        .server = mcp, .ephemeral_port = true, .max_clients = 5,
        .sse_write_timeout_ms = 1000, .sse_sndbuf = 8192,
    };
    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);
    wire_server_to_http(mcp, http);
    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t server_tid, reader_tid;
    MD_CHECK(pthread_create(&server_tid, NULL, server_thread, &sa) == 0);

    char headers[1024];
    int stalled[4];
    int stalled_rcvbuf = 0;
    for (int i = 0; i < 4; i++) {
        stalled[i] = sse_connect_opts(port, 1024, headers, sizeof(headers));
        MD_CHECK(stalled[i] >= 0);
        int actual = 0;
        socklen_t optlen = sizeof(actual);
        MD_CHECK(getsockopt(stalled[i], SOL_SOCKET, SO_RCVBUF, &actual, &optlen) == 0);
        MD_CHECK(actual > 0);
        if (actual > stalled_rcvbuf) stalled_rcvbuf = actual;
        wait_sse_ready(http, stalled[i], i + 1);
    }
    int healthy = sse_connect_opts(port, 8 * 1024 * 1024, headers, sizeof(headers));
    MD_CHECK(healthy >= 0);
    int healthy_rcvbuf = 0;
    socklen_t optlen = sizeof(healthy_rcvbuf);
    MD_CHECK(getsockopt(healthy, SOL_SOCKET, SO_RCVBUF, &healthy_rcvbuf, &optlen) == 0);
    MD_CHECK(healthy_rcvbuf > 0);
    wait_sse_ready(http, healthy, 5);
    MD_CHECK(md_mcp_http_sse_client_count(http) == 5);
    const size_t max_frames = 1024;
    /* Darwin expands even an explicitly tiny loopback receive buffer to
     * about 256 KiB. Overshoot that plus the bounded send buffer in one frame. */
    const size_t len = 512 * 1024;
    char *json = malloc(len + 1);
    MD_CHECK(json != NULL);
    memset(json, 'x', len);
    json[0] = '"'; json[len - 1] = '"'; json[len] = '\0';
    char *frame = malloc(len + 32);
    MD_CHECK(frame != NULL);
    int frame_len = snprintf(frame, len + 32, "event: message\ndata: %s\n\n", json);
    int server_sndbuf = 0;
    /* Allow kernel rounding/doubling, but bound every socket well below one
     * frame, including the healthy client's, via locked getsockopt readback. */
    for (int i = 0; i < 5; i++) {
        int actual = md_mcp_http_sse_client_sndbuf(http, i);
        MD_CHECK(actual > 0 && actual <= 2 * cfg.sse_sndbuf && actual < frame_len);
        if (actual > server_sndbuf) server_sndbuf = actual;
    }
    SseReader reader = { .fd = healthy, .frame = frame,
                         .frame_len = (size_t)frame_len };
    /* Discard readiness leftovers on every socket before the data reader
     * starts; no further readiness probes are sent during the broadcast. */
    for (int i = 0; i < 5; i++) {
        int fd = i < 4 ? stalled[i] : healthy;
        char buf[1024];
        while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
    }
    MD_CHECK(pthread_create(&reader_tid, NULL, read_sse_frames, &reader) == 0);
    size_t sent = 0;
    int clients = md_mcp_http_sse_client_count(http);
    /* Fill both receive and bounded server send buffers. Only the healthy
     * client is drained; the frame cap remains a liveness safety net. */
    while (clients == 5 && sent < max_frames) {
        int before = md_mcp_http_sse_client_count(http);
        MD_CHECK(md_mcp_http_write(json, len, http) == 0);
        sent++;
        clients = md_mcp_http_sse_client_count(http);
        /* A single whole-broadcast deadline drops all four stalled clients
         * in one call, never leaving 2, 3 or 4 between writes. */
        MD_CHECK(before == 5 && (clients == 5 || clients == 1));
    }
    MD_CHECK(md_mcp_http_sse_client_count(http) == 1);
    printf("  SSE buffers: stalled=%d healthy=%d server_sndbuf=%d frames=%zu broadcast=%zu bytes clients=%d\n",
           stalled_rcvbuf, healthy_rcvbuf, server_sndbuf, sent, reader.frame_len * sent, clients);
    fflush(stdout);
    MD_CHECK(md_mcp_http_send_sse(http, "done", "", 0) == 0);
    MD_CHECK(pthread_join(reader_tid, NULL) == 0);
    MD_CHECK(reader.matched);
    MD_CHECK(reader.received == sent);

    /* EOF only confirms the authoritative count assertion above. Do not
     * spend drain budgets on clients already known to have survived. */
    for (int i = 0; clients == 1 && i < 4; i++) {
        size_t queued = 0;
        int64_t drain_deadline = md_now_ms() + 2000;
        bool dropped = false;
        for (;;) {
            int64_t remaining = drain_deadline - md_now_ms();
            if (remaining <= 0) break;
            int readable = md_test_wait_readable(stalled[i], (int)remaining);
            if (readable == 0) continue;
            if (readable < 0) break;
            char buf[16384];
            ssize_t n = read(stalled[i], buf, sizeof(buf));
            if (n == 0) { dropped = true; break; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) break;
            queued += (size_t)n;
        }
        MD_CHECK(dropped);
        MD_CHECK(queued < reader.frame_len * sent);
    }

    /* Both the healthy stream and HTTP request dispatch remain usable. */
    MD_CHECK(md_mcp_http_write("{}", 2, http) == 0);
    SseReader final = { .fd = healthy, .frame = "event: message\ndata: {}\n\n",
                       .frame_len = strlen("event: message\ndata: {}\n\n"), .count = 1 };
    read_sse_frames(&final);
    MD_CHECK(final.matched);
    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"ping\"}";
    char request[512], response[2048];
    snprintf(request, sizeof(request),
             "POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    MD_CHECK(http_request(port, request, response, sizeof(response)) > 0);
    MD_CHECK(strstr(response, "200 OK") && strstr(response, "\"id\":42") &&
             strstr(response, "\"result\""));

    free(frame);
    free(json);
    for (int i = 0; i < 4; i++) close(stalled[i]);
    close(healthy);
    shutdown_http(http);
    MD_CHECK(md_mcp_http_sse_client_count(http) == 0);
    MD_CHECK(pthread_join(server_tid, NULL) == 0);
    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);
    PASS("stalled SSE clients dropped together; healthy stream and POST survive");
}

static cJSON *large_resource(void *userdata)
{
    (void)userdata;
    size_t len = 16 * 1024 * 1024;
    char *text = malloc(len + 1);
    if (!text) return NULL;
    memset(text, 'x', len);
    text[len] = '\0';
    cJSON *contents = cJSON_CreateArray();
    cJSON_AddItemToArray(contents, cJSON_CreateString(text));
    free(text);
    return contents;
}

static void test_stalled_post_destroy(void)
{
    MdMcpServer *mcp = make_mcp_server();
    md_test_mcp_handshake(mcp);
    MdMcpResource resource = {
        .uri = "test://large", .name = "large", .read_handler = large_resource,
    };
    MD_CHECK(md_mcp_server_register_resource(mcp, &resource) == 0);
    MdMcpHttpConfig cfg = { .server = mcp, .ephemeral_port = true, .sse_write_timeout_ms = 200 };
    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);
    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    MD_CHECK(pthread_create(&tid, NULL, server_thread, &sa) == 0);
    int fd = tcp_connect(port), rcvbuf = 1024;
    MD_CHECK(fd >= 0);
    MD_CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == 0);
    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"resources/read\","
                       "\"params\":{\"uri\":\"test://large\"}}";
    char request[512], headers[1024] = {0};
    snprintf(request, sizeof(request),
             "POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    MD_CHECK(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    size_t n = 0;
    /* Read only the header as the handshake; never drain the large body. */
    while (n + 1 < sizeof(headers)) {
        if (md_test_wait_readable(fd, 3000) != 1 || read(fd, headers + n, 1) != 1) break;
        headers[++n] = '\0';
        if (strstr(headers, "\r\n\r\n")) break;
    }
    MD_CHECK(strstr(headers, "200 OK") != NULL);
    shutdown_http(http);
    MD_CHECK(pthread_join(tid, NULL) == 0);
    MD_CHECK(sa.result == 0);
    /* Completion is the assertion; Meson's suite timeout guards liveness. */
    md_mcp_http_destroy(http); /* must drain the stalled POST handler */
    close(fd);
    md_mcp_server_destroy(mcp);
    PASS("stalled large POST response cannot wedge transport destroy");
}

/* ── Test: shutdown flag stops run loop ──────────────────────── */

static void test_shutdown(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);

    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);

    /* Shutdown should cause run to return */
    shutdown_http(http);
    pthread_join(tid, NULL);
    MD_CHECK(sa.result == 0); /* clean shutdown */

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("shutdown");
}

static void test_shutdown_closes_sse_clients(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);

    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);

    char headers[1024];
    int fd = sse_connect(port, headers, sizeof(headers));
    MD_CHECK(fd >= 0);
    MD_CHECK(strstr(headers, "text/event-stream") != NULL);

    shutdown_http(http);
    pthread_join(tid, NULL);
    MD_CHECK(sa.result == 0);

    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf));
    MD_CHECK(n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNRESET)));
    close(fd);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("shutdown closes SSE clients");
}

/* ── Test: run with NULL returns error ───────────────────────── */

static void test_run_null(void)
{
    MD_CHECK(md_mcp_http_run(NULL) == -1);
    PASS("run null");
}

/* ── Test: SSE send with no clients ──────────────────────────── */

static void test_sse_no_clients(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MD_CHECK(mcp != NULL);

    MdMcpHttpConfig cfg = {
        .server = mcp,
        .ephemeral_port = true,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);

    /* SSE send with no clients should succeed (no-op) */
    int ret = md_mcp_http_send_sse(http, "test", "data", 4);
    MD_CHECK(ret == 0);

    /* NULL args */
    MD_CHECK(md_mcp_http_send_sse(NULL, "e", "d", 1) == -1);
    MD_CHECK(md_mcp_http_send_sse(http, NULL, NULL, 0) == -1);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("SSE send no clients");
}

/* Auth tests use real signed events and raw HTTP, not mocked validation. */
static const char auth_sk[] = "0000000000000000000000000000000000000000000000000000000000000001";
static const char auth_body[] = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":9}";

static char *test_npub(void)
{
    char *hex = nostr_key_get_public(auth_sk);
    MD_CHECK(hex != NULL);
    unsigned char pk[32];
    for (int i = 0; i < 32; i++) {
        unsigned value;
        MD_CHECK(sscanf(hex + 2*i, "%2x", &value) == 1);
        pk[i] = (unsigned char)value;
    }
    free(hex);
    char *npub = NULL;
    MD_CHECK(nostr_nip19_encode_npub(pk, &npub) == 0);
    return npub;
}

static char *signed_auth(const char *url, const char *method, const char *payload,
                         int age, bool bad_sig, bool unknown, bool wrong_kind)
{
    char hash_hex[65];
    if (payload) {
        unsigned char hash[32];
        SHA256((const unsigned char *)payload, strlen(payload), hash);
        for (int i = 0; i < 32; i++) snprintf(hash_hex + 2*i, 3, "%02x", hash[i]);
    }
    NostrEvent *event = nostr_nip98_create_auth_event(url, method, payload ? hash_hex : NULL);
    MD_CHECK(event != NULL);
    nostr_event_set_created_at(event, (int64_t)time(NULL) + age);
    /* Unique content keeps independently signed requests distinct even in the
     * same second. Replaying a header below still reuses the exact event id. */
    static unsigned nonce;
    char content[384];
    snprintf(content, sizeof(content), "auth-test-%u-%0320u", ++nonce, 0);
    nostr_event_set_content(event, content);
    if (wrong_kind) nostr_event_set_kind(event, 1);
    const char *sk2 = "0000000000000000000000000000000000000000000000000000000000000002";
    MD_CHECK(nostr_event_sign(event, unknown ? sk2 : auth_sk) == 0);
    if (bad_sig) nostr_event_set_sig(event,
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000");
    char *header = nostr_nip98_create_auth_header(event);
    nostr_event_free(event);
    MD_CHECK(header != NULL);
    return header;
}

/* Read through headers only: successful SSE must not wait for a close. */
static int auth_status(uint16_t port, const char *method, const char *path,
                       const char *host, const char *auth, const char *extra)
{
    char request[8192], response[1024] = {0};
    int len = snprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\n%s%s%s%s%s%s"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        method, path, host ? "Host: " : "", host ? host : "", host ? "\r\n" : "",
        auth ? "Authorization: " : "", auth ? auth : "", auth ? "\r\n" : "",
        strcmp(method, "POST") == 0 ? strlen(auth_body) : 0,
        strcmp(method, "POST") == 0 ? auth_body : "");
    MD_CHECK(len > 0 && len < (int)sizeof(request));
    if (extra) {
        char *headers_end = strstr(request, "\r\n\r\n");
        size_t offset = (size_t)(headers_end + 2 - request), added = strlen(extra);
        MD_CHECK((size_t)len + added < sizeof(request));
        memmove(request + offset + added, request + offset, (size_t)len - offset + 1);
        memcpy(request + offset, extra, added);
        len += (int)added;
    }
    int fd = tcp_connect(port);
    MD_CHECK(fd >= 0);
    if (fd < 0) return -1;
    int64_t deadline = md_now_ms() + 3000;
    MD_CHECK(md_fd_write_deadline(fd, request, (size_t)len, deadline, true, NULL) == 0);
    size_t total = 0;
    while (!strstr(response, "\r\n\r\n") && total < sizeof(response)-1) {
        int64_t left = deadline - md_now_ms();
        if (left <= 0 || md_test_wait_readable(fd, (int)left) != 1) break;
        ssize_t n = read(fd, response + total, sizeof(response)-1-total);
        if (n <= 0) break;
        total += (size_t)n;
        response[total] = 0;
    }
    close(fd);
    int status = -1;
    MD_CHECK(sscanf(response, "HTTP/1.1 %d", &status) == 1);
    return status;
}

static void test_auth_configuration(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MdMcpHttpConfig cfg = { .server = mcp, .ephemeral_port = true, .bind_addr = "0.0.0.0" };
    MD_CHECK(md_mcp_http_create(&cfg) == NULL);
    cfg.bind_addr = "::";
    MD_CHECK(md_mcp_http_create(&cfg) == NULL);
    cfg.bind_addr = NULL;
    char token_path[] = "/tmp/md-http-token-XXXXXX";
    int fd = mkstemp(token_path);
    MD_CHECK(fd >= 0);
    MD_CHECK(write(fd, "test-secret\n", 12) == 12);
    close(fd);
    cfg.token_file = token_path;
    MD_CHECK(chmod(token_path, 0644) == 0);
    MD_CHECK(md_mcp_http_create(&cfg) == NULL);
    MD_CHECK(chmod(token_path, 0600) == 0);
    FILE *token_file = fopen(token_path, "wb");
    MD_CHECK(token_file != NULL);
    const char malformed[] = "a\0long-secret\n";
    MD_CHECK(fwrite(malformed, 1, sizeof(malformed)-1, token_file) == sizeof(malformed)-1);
    fclose(token_file);
    MD_CHECK(md_mcp_http_create(&cfg) == NULL);
    token_file = fopen(token_path, "w");
    MD_CHECK(token_file != NULL);
    MD_CHECK(fputs("test-secret\n", token_file) >= 0);
    fclose(token_file);
    cfg.bind_addr = "0.0.0.0";
    MdMcpHttp *http = md_mcp_http_create(&cfg);
    MD_CHECK(http != NULL);
    if (!http) {
        unlink(token_path);
        md_mcp_server_destroy(mcp);
        return;
    }
    md_mcp_http_destroy(http);
    cfg.bind_addr = NULL;
    cfg.token_file = "/nonexistent/md-token";
    MD_CHECK(md_mcp_http_create(&cfg) == NULL);
    cfg.token_file = NULL;
    const char *invalid_npub[] = {"npub1invalid"};
    cfg.allow_npubs = invalid_npub; cfg.allow_npub_count = 1;
    MD_CHECK(md_mcp_http_create(&cfg) == NULL);
    cfg.allow_npub_count = 0;
    const char *bad_urls[] = {"https://example.com/mcp?x=1", "https://user@example.com/mcp",
                              "https://example.com/other", "https://example.com:99999/mcp"};
    cfg.public_url_count = 1;
    for (size_t i = 0; i < sizeof(bad_urls)/sizeof(bad_urls[0]); i++) {
        cfg.public_urls = &bad_urls[i];
        MD_CHECK(md_mcp_http_create(&cfg) == NULL);
    }
    cfg.public_url_count = 0;
    char allow_path[] = "/tmp/md-http-allow-XXXXXX";
    fd = mkstemp(allow_path);
    MD_CHECK(fd >= 0);
    close(fd);
    cfg.allow_file = allow_path;
    MD_CHECK(md_mcp_http_create(&cfg) == NULL); /* explicit empty list fails closed */
    FILE *file = fopen(allow_path, "w");
    char *npub = test_npub();
    fprintf(file, "# allowed agent\n\n%s\n", npub);
    fclose(file);
    cfg.bind_addr = "0.0.0.0";
    http = md_mcp_http_create(&cfg);
    free(npub);
    unlink(token_path); unlink(allow_path);
    REQUIRE_HTTP(http, mcp);
    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);
    PASS("auth startup credentials, permissions, URL validation and allow-file");
}

static void test_http_auth(void)
{
    MdMcpServer *mcp = make_mcp_server();
    char token_path[] = "/tmp/md-http-auth-XXXXXX";
    int fd = mkstemp(token_path);
    MD_CHECK(fd >= 0);
    MD_CHECK(write(fd, "test-secret\n", 12) == 12);
    close(fd);
    char *npub = test_npub();
    const char *npubs[] = {npub};
    const char *urls[] = {"https://agent.example/mcp", "http://proxy.example:8000/mcp"};
    MdMcpHttpConfig cfg = { .server = mcp, .ephemeral_port = true, .token_file = token_path,
        .allow_npubs = npubs, .allow_npub_count = 1,
        .public_urls = urls, .public_url_count = 2 };
    MdMcpHttp *http = md_mcp_http_create(&cfg);
    /* Config is owned by the server, not borrowed from its caller/files. */
    unlink(token_path); free(npub);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);
    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);
    const char *methods[] = {"POST", "GET"};
    for (int m = 0; m < 2; m++) {
        const char *method = methods[m];
        MD_CHECK(auth_status(port, method, "/mcp", "localhost", NULL, NULL) == 401);
        MD_CHECK(auth_status(port, method, "/mcp", "localhost", "Bearer wrong", NULL) == 401);
        MD_CHECK(auth_status(port, method, "/mcp", "localhost", "Basic abc", NULL) == 401);
        MD_CHECK(auth_status(port, method, "/mcp", "localhost", "Nostr invalid", NULL) == 401);
        MD_CHECK(auth_status(port, method, "/mcp", "evil.example", "Bearer test-secret", NULL) == 403);
        MD_CHECK(auth_status(port, method, "/mcp", NULL, "Bearer test-secret", NULL) == 403);
        MD_CHECK(auth_status(port, method, "/mcp", "localhost.evil", "Bearer test-secret", NULL) == 403);
        MD_CHECK(auth_status(port, method, "/mcp", "localhost", "Bearer test-secret", "oRiGiN:\r\n") == 403);
        MD_CHECK(auth_status(port, method, "/mcp", "localhost", "Bearer test-secret",
                             "Origin: http://localhost\r\n") == 403);
        MD_CHECK(auth_status(port, method, "/mcp", "localhost", "Bearer test-secret",
                             "Host: evil.example\r\n") == 403);
        MD_CHECK(auth_status(port, method, "/mcp", "localhost", "Bearer test-secret",
                             "Authorization: Bearer test-secret\r\n") == 401);
        const struct { const char *url, *signed_method, *payload; int age; bool sig, unknown, kind; } invalid[] = {
            {"https://evil.example/mcp", method, auth_body, 0, false, false, false},
            {urls[0], m ? "POST" : "GET", auth_body, 0, false, false, false},
            {urls[0], method, auth_body, -120, false, false, false},
            {urls[0], method, auth_body, 120, false, false, false},
            {urls[0], method, auth_body, 0, true, false, false},
            {urls[0], method, auth_body, 0, false, true, false},
            {urls[0], method, auth_body, 0, false, false, true},
        };
        for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
            char *header = signed_auth(invalid[i].url, invalid[i].signed_method, invalid[i].payload,
                                      invalid[i].age, invalid[i].sig, invalid[i].unknown, invalid[i].kind);
            MD_CHECK(auth_status(port, method, "/mcp", "agent.example", header, NULL) == 401);
            free(header);
        }
        char *header = signed_auth(urls[0], method, auth_body, 0, false, false, false);
        /* Large signed header fits the existing 4096-byte read chunk. */
        printf("  %s signed Authorization header: %zu bytes\n", method, strlen(header));
        MD_CHECK(strlen(header) >= 1000 && strlen(header) < 1400);
        MD_CHECK(auth_status(port, method, "/mcp", "agent.example:443", header, NULL) == 200);
        MD_CHECK(auth_status(port, method, "/mcp", "agent.example", header, NULL) == 401);
        free(header);
        MD_CHECK(auth_status(port, method, "/mcp", "localhost", "Bearer test-secret", NULL) == 200);
    }
    /* POST requires payload, unlike bodyless SSE. */
    char *header = signed_auth(urls[0], "POST", NULL, 0, false, false, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "agent.example", header, NULL) == 401);
    free(header);
    header = signed_auth(urls[0], "POST", "different body", 0, false, false, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "agent.example", header, NULL) == 401);
    free(header);
    header = signed_auth(urls[1], "POST", auth_body, 0, false, false, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "PrOxY.Example:8000", header, NULL) == 200);
    free(header);
    MD_CHECK(auth_status(port, "POST", "/mcp", "proxy.example:8001", "Bearer test-secret", NULL) == 403);
    MD_CHECK(auth_status(port, "POST", "/mcp", "proxy.example", "Bearer test-secret", NULL) == 403);
    const char *forms[] = {"localhost", "127.0.0.1", "[::1]"};
    for (size_t i = 0; i < 3; i++) {
        char url[128];
        snprintf(url, sizeof(url), "http://%s:%u/mcp", forms[i], port);
        header = signed_auth(url, "POST", auth_body, 0, false, false, false);
        MD_CHECK(auth_status(port, "POST", "/mcp", forms[i], header, NULL) == 200);
        free(header);
    }
    /* Health probes remain usable without auth and never reserve an SSE slot. */
    int clients = md_mcp_http_sse_client_count(http);
    for (int i = 0; i < 5; i++)
        MD_CHECK(auth_status(port, "GET", "/healthz", "localhost", NULL, NULL) == 200);
    MD_CHECK(md_mcp_http_sse_client_count(http) == clients);
    MD_CHECK(auth_status(port, "GET", "/healthz", "evil.example", NULL, NULL) == 403);
    shutdown_http(http);
    pthread_join(tid, NULL);
    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);
    PASS("bearer and NIP-98 POST/SSE authentication, replay, payload, Host/Origin, healthz");
}

/* ── Live NIP-98 authorizer (md-ah9.3) ───────────────────────── */

static const char auth_sk2[] = "0000000000000000000000000000000000000000000000000000000000000002";

typedef struct {
    pthread_mutex_t mu;
    char allowed[65];   /* empty: source not ready / nobody granted */
    unsigned calls;
} LiveAcl;

static bool live_acl_authorize(const char *pk, void *ud)
{
    LiveAcl *acl = ud;
    pthread_mutex_lock(&acl->mu);
    acl->calls++;
    bool ok = acl->allowed[0] && strcmp(pk, acl->allowed) == 0;
    pthread_mutex_unlock(&acl->mu);
    return ok;
}

static void live_acl_set(LiveAcl *acl, const char *pk)
{
    pthread_mutex_lock(&acl->mu);
    snprintf(acl->allowed, sizeof(acl->allowed), "%s", pk ? pk : "");
    pthread_mutex_unlock(&acl->mu);
}

static unsigned live_acl_calls(LiveAcl *acl)
{
    pthread_mutex_lock(&acl->mu);
    unsigned calls = acl->calls;
    pthread_mutex_unlock(&acl->mu);
    return calls;
}

static char *npub_for(const char *sk)
{
    char *hex = nostr_key_get_public(sk);
    MD_CHECK(hex != NULL);
    unsigned char pk[32];
    for (int i = 0; i < 32; i++) {
        unsigned value;
        MD_CHECK(sscanf(hex + 2*i, "%2x", &value) == 1);
        pk[i] = (unsigned char)value;
    }
    free(hex);
    char *npub = NULL;
    MD_CHECK(nostr_nip19_encode_npub(pk, &npub) == 0);
    return npub;
}

/* Signed loopback NIP-98 header for key 1 (default) or key 2. */
static char *loopback_auth(uint16_t port, const char *method, bool key2, bool bad_sig)
{
    char url[96];
    snprintf(url, sizeof(url), "http://localhost:%u/mcp", port);
    return signed_auth(url, method, strcmp(method, "POST") == 0 ? auth_body : NULL,
                       0, bad_sig, key2, false);
}

/* GET /mcp with Authorization; returns an fd positioned after the headers
 * of a 200 response, or -1. */
static int sse_connect_auth(uint16_t port, const char *auth)
{
    int fd = tcp_connect(port);
    if (fd < 0) return -1;
    char request[4096];
    int len = snprintf(request, sizeof(request),
        "GET /mcp HTTP/1.1\r\nHost: localhost\r\nAuthorization: %s\r\n\r\n", auth);
    int64_t deadline = md_now_ms() + 3000;
    if (len <= 0 || len >= (int)sizeof(request) ||
        md_fd_write_deadline(fd, request, (size_t)len, deadline, true, NULL) != 0) {
        close(fd);
        return -1;
    }
    char headers[1024] = {0};
    size_t total = 0;
    while (!strstr(headers, "\r\n\r\n") && total < sizeof(headers) - 1) {
        int64_t left = deadline - md_now_ms();
        if (left <= 0 || md_test_wait_readable(fd, (int)left) != 1) break;
        ssize_t n = read(fd, headers + total, 1); /* stop exactly at the blank line */
        if (n <= 0) break;
        total += (size_t)n;
    }
    if (strncmp(headers, "HTTP/1.1 200", 12) != 0) { close(fd); return -1; }
    return fd;
}

/* Read one SSE frame (to the blank line); false on EOF/timeout. */
static bool sse_read_frame(int fd, char *buf, size_t cap)
{
    size_t total = 0;
    buf[0] = 0;
    int64_t deadline = md_now_ms() + 3000;
    while (!strstr(buf, "\n\n") && total < cap - 1) {
        int64_t left = deadline - md_now_ms();
        if (left <= 0 || md_test_wait_readable(fd, (int)left) != 1) return false;
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) return false;
        total += (size_t)n;
        buf[total] = 0;
    }
    return strstr(buf, "\n\n") != NULL;
}

static void test_live_authorizer(void)
{
    char *pk1 = nostr_key_get_public(auth_sk);
    char *pk2 = nostr_key_get_public(auth_sk2);
    char *npub2 = npub_for(auth_sk2);
    MD_CHECK(pk1 && pk2 && npub2);
    LiveAcl acl = { .mu = PTHREAD_MUTEX_INITIALIZER };
    MdMcpServer *mcp = make_mcp_server();

    /* The live source alone is a credential: non-loopback binds may start. */
    MdMcpHttpConfig cfg = { .server = mcp, .ephemeral_port = true, .bind_addr = "0.0.0.0",
        .nip98_authorize = live_acl_authorize, .nip98_authorize_userdata = &acl };
    MdMcpHttp *http = md_mcp_http_create(&cfg);
    MD_CHECK(http != NULL);
    md_mcp_http_destroy(http);

    cfg.bind_addr = NULL;
    http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    wire_server_to_http(mcp, http);
    uint16_t port = md_mcp_http_port(http);
    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);

    /* Not ready / no grant: fail closed, and credentials stay mandatory. */
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", NULL, NULL) == 401);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", "Bearer x", NULL) == 401);
    char *h = loopback_auth(port, "POST", false, false);
    unsigned before = live_acl_calls(&acl);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 401);
    MD_CHECK(live_acl_calls(&acl) == before + 1);
    free(h);

    /* A grant takes effect on the next request, without a restart. */
    live_acl_set(&acl, pk1);
    h = loopback_auth(port, "POST", false, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 200);
    free(h);
    h = loopback_auth(port, "POST", true, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 401);
    free(h);

    /* Unvalidated events never reach the live source. */
    before = live_acl_calls(&acl);
    h = loopback_auth(port, "POST", false, true);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 401);
    free(h);
    char wrong_url[96];
    snprintf(wrong_url, sizeof(wrong_url), "http://localhost:%u/mcp", port + 1);
    h = signed_auth(wrong_url, "POST", auth_body, 0, false, false, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 401);
    free(h);
    h = signed_auth("http://localhost/x", "POST", "other", 0, false, false, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 401);
    free(h);
    MD_CHECK(live_acl_calls(&acl) == before);

    /* SSE admitted by the live source is dropped at the next broadcast after
     * revocation; the notification itself is withheld. */
    h = loopback_auth(port, "GET", false, false);
    int sse = sse_connect_auth(port, h);
    free(h);
    MD_CHECK(sse >= 0);
    if (sse >= 0) {
        wait_sse_ready(http, sse, 1);
        char frame[512];
        MD_CHECK(md_mcp_http_send_sse(http, "message", "{\"n\":1}", 7) == 0);
        MD_CHECK(sse_read_frame(sse, frame, sizeof(frame)) && strstr(frame, "{\"n\":1}"));
        live_acl_set(&acl, NULL);
        MD_CHECK(md_mcp_http_send_sse(http, "message", "{\"n\":2}", 7) == 0);
        MD_CHECK(md_mcp_http_sse_client_count(http) == 0);
        MD_CHECK(!sse_read_frame(sse, frame, sizeof(frame)) && !strstr(frame, "n\":2"));
        close(sse);
    }
    h = loopback_auth(port, "POST", false, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 401);
    free(h);
    shutdown_http(http);
    pthread_join(tid, NULL);
    md_mcp_http_destroy(http);

    /* Union with the static allowlist: either source admits; the static
     * set is never revoked by the live one. */
    const char *npubs[] = { npub2 };
    cfg.allow_npubs = npubs;
    cfg.allow_npub_count = 1;
    live_acl_set(&acl, pk1);
    http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    wire_server_to_http(mcp, http);
    port = md_mcp_http_port(http);
    sa = (ServerArgs){ .http = http, .result = -1 };
    pthread_create(&tid, NULL, server_thread, &sa);
    for (int key2 = 0; key2 < 2; key2++) {
        h = loopback_auth(port, "POST", key2, false);
        MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 200);
        free(h);
    }
    before = live_acl_calls(&acl);
    h = loopback_auth(port, "GET", true, false);
    sse = sse_connect_auth(port, h);
    free(h);
    MD_CHECK(sse >= 0);
    MD_CHECK(live_acl_calls(&acl) == before); /* static hit: live source unused */
    live_acl_set(&acl, NULL);
    h = loopback_auth(port, "POST", false, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 401);
    free(h);
    h = loopback_auth(port, "POST", true, false);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", h, NULL) == 200);
    free(h);
    if (sse >= 0) {
        wait_sse_ready(http, sse, 1);
        char frame[512];
        MD_CHECK(md_mcp_http_send_sse(http, "message", "{\"n\":3}", 7) == 0);
        MD_CHECK(sse_read_frame(sse, frame, sizeof(frame)) && strstr(frame, "{\"n\":3}"));
        MD_CHECK(md_mcp_http_sse_client_count(http) == 1);
        close(sse);
    }
    shutdown_http(http);
    pthread_join(tid, NULL);
    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);
    free(pk1); free(pk2); free(npub2);
    PASS("live NIP-98 authorizer: fail-closed, live grant/revoke, SSE drop, static union");
}

static void test_unauthenticated_guards_and_read_budget(void)
{
    MdMcpServer *mcp = make_mcp_server();
    MdMcpHttpConfig cfg = { .server = mcp, .ephemeral_port = true };
    MdMcpHttp *http = md_mcp_http_create(&cfg);
    REQUIRE_HTTP(http, mcp);
    uint16_t port = md_mcp_http_port(http);
    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);
    MD_CHECK(auth_status(port, "POST", "/mcp", "localhost", NULL, NULL) == 200);
    MD_CHECK(auth_status(port, "POST", "/mcp", "foreign.example", NULL, NULL) == 403);
    MD_CHECK(auth_status(port, "GET", "/mcp", "localhost", NULL, "Origin: null\r\n") == 403);
    MD_CHECK(auth_status(port, "GET", "/healthz", "localhost", NULL, NULL) == 200);
    /* No Host hidden inside X-Host; no Content-Length hidden in X-* either. */
    MD_CHECK(auth_status(port, "POST", "/mcp", NULL, NULL, "X-Host: localhost\r\n") == 403);
    int fd = tcp_connect(port);
    MD_CHECK(fd >= 0);
    const char *partial = "POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n{";
    int64_t start = md_now_ms();
    MD_CHECK(write(fd, partial, strlen(partial)) == (ssize_t)strlen(partial));
    MD_CHECK(md_test_wait_readable(fd, 3000) == 1);
    char response[1024] = {0};
    ssize_t n = read(fd, response, sizeof(response)-1);
    MD_CHECK(n > 0 && strstr(response, "400 Bad Request") != NULL);
    MD_CHECK(md_now_ms() - start < 3000);
    close(fd);
    shutdown_http(http);
    pthread_join(tid, NULL);
    md_mcp_http_destroy(http);
    MD_CHECK(md_now_ms() - start < 5000);
    md_mcp_server_destroy(mcp);
    PASS("always-on guards without credentials and read/shutdown budget");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_mcp_http: HTTP+SSE transport tests\n");

    test_auth_configuration();
    test_http_auth();
    test_live_authorizer();
    test_unauthenticated_guards_and_read_budget();
    test_create_null();
    test_create_invalid_bind_addr();
    test_create_destroy();
    test_destroy_null();
    test_run_null();
    test_sse_no_clients();
    test_post_initialize();
    test_invalid_content_length_rejected();
    test_post_requires_json_content_type();
    test_get_404();
    test_sse_multiline_data();
    test_sse_stalled_client();
    test_stalled_post_destroy();
    test_sse_max_clients_config();
    test_shutdown();
    test_shutdown_closes_sse_clients();

    printf("\nHTTP transport tests finished.\n");
    return md_test_report();
}
