/* Native CRT pipes: framing, EOF, stalled/broken output and shutdown ownership. */
#include "mcp_stdio.h"
#include "mcp_server.h"
#include "md_test.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

typedef struct { MdMcpStdio *ctx; HANDLE done; int result; } Runner;
static void *run_stdio(void *userdata) {
    Runner *r = userdata;
    r->result = md_mcp_stdio_run(r->ctx);
    SetEvent(r->done);
    return NULL;
}
static int unused_write(const char *json, size_t len, void *ud) {
    (void)json; (void)len; (void)ud; return -1;
}
static MdMcpServer *server_new(void) {
    MdMcpServerConfig cfg = { .server_name = "native-pipe", .server_version = "1",
                             .write_fn = unused_write };
    MdMcpServer *server = md_mcp_server_create(&cfg);
    MD_CHECK(server != NULL);
    return server;
}
static void make_pipe(int pair[2]) {
    MD_CHECK(_pipe(pair, 4096, _O_BINARY | _O_NOINHERIT) == 0);
}
static void test_framing(void) {
    int in[2], out[2]; make_pipe(in); make_pipe(out);
    MdMcpServer *server = server_new();
    MdMcpStdio *ctx = md_mcp_stdio_create(server, in[0], out[1]);
    MD_CHECK(ctx != NULL);
    Runner r = { .ctx = ctx, .done = CreateEventW(NULL, TRUE, FALSE, NULL) };
    pthread_t thread; MD_CHECK(pthread_create(&thread, NULL, run_stdio, &r) == 0);
    const char request[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n"
                           "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n";
    MD_CHECK(_write(in[1], request, sizeof(request) - 1) == sizeof(request) - 1);
    _close(in[1]);
    MD_CHECK(WaitForSingleObject(r.done, 5000) == WAIT_OBJECT_0);
    pthread_join(thread, NULL); MD_CHECK(r.result == 0);
    _close(out[1]);
    char response[4096];
    ssize_t n = md_fd_read_timeout(out[0], response, sizeof(response) - 1, 2000);
    MD_CHECK(n > 0);
    if (n > 0) {
        response[n] = 0;
        MD_CHECK(strstr(response, "\"id\":1") != NULL);
        MD_CHECK(strstr(response, "\"id\":2") != NULL);
        MD_CHECK(strchr(response, '\r') == NULL);
        MD_CHECK(response[n-1] == '\n');
    }
    CloseHandle(r.done); _close(in[0]); _close(out[0]);
    md_mcp_stdio_destroy(ctx); md_mcp_server_destroy(server);
}
static void test_shutdown(bool partial) {
    int in[2], out[2]; make_pipe(in); make_pipe(out);
    MdMcpServer *server = server_new();
    MdMcpStdio *ctx = md_mcp_stdio_create(server, in[0], out[1]);
    MD_CHECK(ctx != NULL);
    Runner r = { .ctx = ctx, .done = CreateEventW(NULL, TRUE, FALSE, NULL) };
    pthread_t thread; MD_CHECK(pthread_create(&thread, NULL, run_stdio, &r) == 0);
    if (partial) MD_CHECK(_write(in[1], "{", 1) == 1);
    md_mcp_stdio_shutdown(ctx);
    MD_CHECK(WaitForSingleObject(r.done, 5000) == WAIT_OBJECT_0);
    pthread_join(thread, NULL); MD_CHECK(r.result == 0);
    /* stdin's original handle remains valid and was not closed to cancel I/O. */
    MD_CHECK(_write(in[1], "x", 1) == 1);
    CloseHandle(r.done); _close(in[0]); _close(in[1]); _close(out[0]); _close(out[1]);
    md_mcp_stdio_destroy(ctx); md_mcp_server_destroy(server);
}
static void test_failed_output(bool broken) {
    int in[2], out[2]; make_pipe(in); make_pipe(out);
    MdMcpServer *server = server_new();
    MdMcpStdio *ctx = md_mcp_stdio_create_ex(server, in[0], out[1], 40);
    MD_CHECK(ctx != NULL);
    if (broken) _close(out[0]);
    size_t size = 1024 * 1024;
    char *message = malloc(size); MD_CHECK(message != NULL); memset(message, 'x', size);
    int64_t started = md_now_ms();
    MD_CHECK(md_mcp_stdio_write(message, size, ctx) < 0);
    /* Generous failure-only bound; no minimum elapsed time encodes success. */
    MD_CHECK(md_now_ms() - started < 5000);
    if (!broken) {
        /* A canceled synchronous write may have committed unknown bytes. Never
         * append another response to such a potentially partial JSON frame. */
        MD_CHECK(md_mcp_stdio_write("{}", 2, ctx) < 0);
        _close(out[0]);
    }
    free(message); _close(in[0]); _close(in[1]); _close(out[1]);
    md_mcp_stdio_destroy(ctx); md_mcp_server_destroy(server);
}
int main(void) {
    test_framing();
    test_shutdown(false); test_shutdown(true);
    for (int i = 0; i < 12; i++) { test_failed_output(false); test_failed_output(true); }
    return md_test_report();
}
