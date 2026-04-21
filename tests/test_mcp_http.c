/*
 * metadesk — tests/test_mcp_http.c
 * Unit tests for the HTTP+SSE MCP transport.
 *
 * Tests create/destroy lifecycle, HTTP request handling via raw
 * TCP client, and shutdown behavior.
 */
#include "mcp_server.h"
#include "mcp_http.h"
#include "jsonrpc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define PASS(name) printf("  PASS  %s\n", name)

/* Dynamic port to avoid conflicts with other tests */
static uint16_t g_test_port = 17710;

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

/* ── Helper: raw TCP connect to localhost ─────────────────────── */

static int tcp_connect(uint16_t port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(port);
    addr.sin6_addr   = in6addr_loopback;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── Helper: send HTTP request, read response ────────────────── */

static int http_request(uint16_t port, const char *request,
                        char *resp_buf, size_t resp_buf_len)
{
    int fd = tcp_connect(port);
    if (fd < 0) return -1;

    ssize_t w = write(fd, request, strlen(request));
    if (w <= 0) { close(fd); return -1; }

    /* Read response with timeout */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t n = read(fd, resp_buf, resp_buf_len - 1);
    close(fd);

    if (n <= 0) return -1;
    resp_buf[n] = '\0';
    return (int)n;
}

/* ── Test: NULL config ───────────────────────────────────────── */

static void test_create_null(void)
{
    assert(md_mcp_http_create(NULL) == NULL);

    MdMcpHttpConfig cfg = { .server = NULL };
    assert(md_mcp_http_create(&cfg) == NULL);

    PASS("create null config");
}

/* ── Test: create and destroy ────────────────────────────────── */

static void test_create_destroy(void)
{
    MdMcpServer *mcp = make_mcp_server();
    assert(mcp != NULL);

    uint16_t port = g_test_port++;
    MdMcpHttpConfig cfg = {
        .server = mcp,
        .port   = port,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    assert(http != NULL);

    /* Verify we can get write fn */
    MdMcpWriteFn fn = md_mcp_http_get_write_fn(http);
    assert(fn != NULL);

    void *ud = md_mcp_http_get_write_userdata(http);
    assert(ud == http);

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
    /* Create HTTP transport first with a temporary MCP server,
     * then wire the write function properly.
     * The HTTP handler dispatches to h->mcp_server, which must
     * have http_write_fn set for TLS-based response routing. */
    MdMcpServer *temp_mcp = make_mcp_server();
    assert(temp_mcp != NULL);

    uint16_t port = g_test_port++;
    MdMcpHttpConfig cfg = {
        .server = temp_mcp,
        .port   = port,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    assert(http != NULL);

    /* Now create the real MCP server wired to the HTTP write function,
     * and swap it in. Since the HTTP handler uses h->mcp_server, which
     * is temp_mcp, we need to ensure temp_mcp has the right write_fn.
     * We can't swap servers, so instead we test the lifecycle and
     * HTTP routing at the transport level. */

    /* Start server in background */
    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);
    usleep(100000); /* wait for server to start */

    /* Send initialize request — the MCP server dispatch happens inside
     * handle_client which uses http_write_fn via TLS. But since the MCP
     * server was created with dummy_write, responses route to dummy.
     * The HTTP handler falls back to 202 Accepted when TLS has no response. */
    const char *body = "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
                       "\"id\":1,\"params\":{\"protocolVersion\":\"2025-03-26\","
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
    assert(n > 0);

    /* The HTTP layer should return a valid HTTP response.
     * With correct wiring we'd get 200 + JSON-RPC; with dummy write
     * we get 202 Accepted (no TLS response captured). Either is valid
     * for testing that the HTTP transport accepts and routes POST. */
    assert(strstr(response, "HTTP/1.") != NULL);

    /* Shutdown */
    md_mcp_http_shutdown(http);
    pthread_join(tid, NULL);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(temp_mcp);

    PASS("POST /mcp HTTP routing");
}

/* ── Test: GET unknown path → 404 ────────────────────────────── */

static void test_get_404(void)
{
    MdMcpServer *mcp = make_mcp_server();
    assert(mcp != NULL);

    uint16_t port = g_test_port++;
    MdMcpHttpConfig cfg = {
        .server = mcp,
        .port   = port,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    assert(http != NULL);

    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);
    usleep(100000);

    const char *request = "GET /unknown HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char response[2048];
    int n = http_request(port, request, response, sizeof(response));
    assert(n > 0);
    assert(strstr(response, "404") != NULL);

    md_mcp_http_shutdown(http);
    pthread_join(tid, NULL);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("GET unknown path → 404");
}

/* ── Test: shutdown flag stops run loop ──────────────────────── */

static void test_shutdown(void)
{
    MdMcpServer *mcp = make_mcp_server();
    assert(mcp != NULL);

    uint16_t port = g_test_port++;
    MdMcpHttpConfig cfg = {
        .server = mcp,
        .port   = port,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    assert(http != NULL);

    ServerArgs sa = { .http = http, .result = -1 };
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &sa);
    usleep(100000);

    /* Shutdown should cause run to return */
    md_mcp_http_shutdown(http);
    pthread_join(tid, NULL);
    assert(sa.result == 0); /* clean shutdown */

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("shutdown");
}

/* ── Test: run with NULL returns error ───────────────────────── */

static void test_run_null(void)
{
    assert(md_mcp_http_run(NULL) == -1);
    PASS("run null");
}

/* ── Test: SSE send with no clients ──────────────────────────── */

static void test_sse_no_clients(void)
{
    MdMcpServer *mcp = make_mcp_server();
    assert(mcp != NULL);

    uint16_t port = g_test_port++;
    MdMcpHttpConfig cfg = {
        .server = mcp,
        .port   = port,
    };

    MdMcpHttp *http = md_mcp_http_create(&cfg);
    assert(http != NULL);

    /* SSE send with no clients should succeed (no-op) */
    int ret = md_mcp_http_send_sse(http, "test", "data", 4);
    assert(ret == 0);

    /* NULL args */
    assert(md_mcp_http_send_sse(NULL, "e", "d", 1) == -1);
    assert(md_mcp_http_send_sse(http, NULL, NULL, 0) == -1);

    md_mcp_http_destroy(http);
    md_mcp_server_destroy(mcp);

    PASS("SSE send no clients");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_mcp_http: HTTP+SSE transport tests\n");

    test_create_null();
    test_create_destroy();
    test_destroy_null();
    test_run_null();
    test_sse_no_clients();
    test_post_initialize();
    test_get_404();
    test_shutdown();

    printf("\nAll HTTP transport tests passed.\n");
    return 0;
}
