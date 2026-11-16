/*
 * metadesk — test_mcp_stdio.c
 * Integration test for MCP stdio transport.
 *
 * Creates a pipe-based MCP server, sends JSON-RPC messages, and
 * verifies responses come back correctly over the pipe.
 */
#include "mcp_server.h"
#include "md_mcp_fixture.h"
#include "mcp_tools.h"
#include "mcp_stdio.h"
#include "jsonrpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include "md_test_net.h"
#include "md_test.h"


/* ── Pipe-based test harness ─────────────────────────────────── */

typedef struct {
    int agent_to_server[2];   /* agent writes, server reads */
    int server_to_agent[2];   /* server writes, agent reads */
    MdMcpServer *server;
    MdMcpStdio  *stdio_ctx;
    MdMcpToolCtx tool_ctx;
    pthread_t    server_thread;
} TestHarness;

static void *server_thread_fn(void *arg)
{
    TestHarness *h = (TestHarness *)arg;
    md_mcp_stdio_run(h->stdio_ctx);
    return NULL;
}

/* Direct pipe write function — writes JSON + newline to the fd stored in userdata */
static int pipe_write_fn(const char *json, size_t len, void *userdata)
{
    int fd = *(int *)userdata;
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, json + total, len - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    char nl = '\n';
    if (write(fd, &nl, 1) != 1) return -1;
    return 0;
}

static TestHarness *harness_create(void)
{
    TestHarness *h = calloc(1, sizeof(*h));
    pipe(h->agent_to_server);
    pipe(h->server_to_agent);

    /* Server writes directly to the pipe — simpler than wiring through stdio */
    MdMcpServerConfig cfg = {
        .server_name = "metadesk-test",
        .server_version = "0.0.1",
        .write_fn = pipe_write_fn,
        .write_userdata = &h->server_to_agent[1],  /* write end of pipe */
    };
    h->server = md_mcp_server_create(&cfg);

    /* stdio transport reads from agent pipe, dispatches to server */
    h->stdio_ctx = md_mcp_stdio_create(h->server, h->agent_to_server[0],
                                        h->server_to_agent[1]);

    /* Register tools */
    md_mcp_register_tools(h->server, &h->tool_ctx);

    return h;
}

static void harness_start(TestHarness *h)
{
    pthread_create(&h->server_thread, NULL, server_thread_fn, h);
}

static void harness_send(TestHarness *h, const char *json)
{
    size_t len = strlen(json);
    write(h->agent_to_server[1], json, len);
    write(h->agent_to_server[1], "\n", 1);
}

static char *harness_recv(TestHarness *h)
{
    /* Read a line from server_to_agent read end */
    char buf[8192];
    size_t pos = 0;

    while (pos < sizeof(buf) - 1) {
        ssize_t n = read(h->server_to_agent[0], buf + pos, 1);
        if (n <= 0) break;
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            return strdup(buf);
        }
        pos++;
    }
    buf[pos] = '\0';
    return pos > 0 ? strdup(buf) : NULL;
}

static void harness_destroy(TestHarness *h)
{
    /* Close agent write end → server sees EOF → run loop exits */
    close(h->agent_to_server[1]);
    pthread_join(h->server_thread, NULL);

    md_mcp_stdio_destroy(h->stdio_ctx);
    close(h->agent_to_server[0]);
    close(h->server_to_agent[0]);
    close(h->server_to_agent[1]);

    md_mcp_tools_cleanup(&h->tool_ctx);
    md_mcp_server_destroy(h->server);
    free(h);
}

/* ── Tests ───────────────────────────────────────────────────── */

static void test_stdio_init_and_ping(void)
{
    TestHarness *h = harness_create();
    harness_start(h);

    /* Send initialize */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
                     "\"id\":1,\"params\":{\"protocolVersion\":\"" MD_TEST_MCP_PROTOCOL "\","
                     "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}");

    char *resp = harness_recv(h);
    MD_CHECK(resp != NULL);
    cJSON *r = cJSON_Parse(resp);
    MD_CHECK(cJSON_GetObjectItem(r, "result") != NULL);
    cJSON_Delete(r);
    free(resp);

    /* Send initialized notification */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

    /* Send ping */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":2}");
    resp = harness_recv(h);
    MD_CHECK(resp != NULL);
    r = cJSON_Parse(resp);
    MD_CHECK(cJSON_GetObjectItem(r, "result") != NULL);
    MD_CHECK(cJSON_GetObjectItem(r, "id")->valueint == 2);
    cJSON_Delete(r);
    free(resp);

    harness_destroy(h);
    PASS("stdio init + ping over pipe");
}

static void test_stdio_tools_list(void)
{
    TestHarness *h = harness_create();
    harness_start(h);

    /* Initialize */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
                     "\"id\":1,\"params\":{}}");
    free(harness_recv(h));  /* consume init response */

    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

    /* List tools */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":2}");
    char *resp = harness_recv(h);
    MD_CHECK(resp != NULL);

    cJSON *r = cJSON_Parse(resp);
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "result"), "tools");
    MD_CHECK(cJSON_GetArraySize(tools) == 14);
    cJSON_Delete(r);
    free(resp);

    harness_destroy(h);
    PASS("stdio tools/list returns 14 tools");
}

static void test_stdio_tool_call(void)
{
    TestHarness *h = harness_create();
    harness_start(h);

    /* Initialize */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
                     "\"id\":1,\"params\":{}}");
    free(harness_recv(h));
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

    /* Call click tool */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":3,"
                     "\"params\":{\"name\":\"metadesk_click\","
                     "\"arguments\":{\"target_id\":\"n42\"}}}");
    char *resp = harness_recv(h);
    MD_CHECK(resp != NULL);

    cJSON *r = cJSON_Parse(resp);
    cJSON *result = cJSON_GetObjectItem(r, "result");
    cJSON *content = cJSON_GetObjectItem(result, "content");
    MD_CHECK(cJSON_IsArray(content));
    MD_CHECK(cJSON_GetArraySize(content) >= 1);
    cJSON_Delete(r);
    free(resp);

    harness_destroy(h);
    PASS("stdio tool call round-trip");
}

static cJSON *count_dispatch(const cJSON *args, bool *is_error,
                             char **error_msg, void *userdata)
{
    (void)args; (void)is_error; (void)error_msg;
    (*(int *)userdata)++;
    return cJSON_CreateArray();
}

static void test_stdio_broken_pipe(void)
{
    int input[2], output[2];
    MD_CHECK(pipe(input) == 0 && pipe(output) == 0);
    MdMcpServerConfig config = { .server_name = "broken-pipe" };
    MdMcpServer *server = md_mcp_server_create(&config);
    MD_CHECK(server != NULL);
    MdMcpStdio *ctx = md_mcp_stdio_create(server, input[0], output[1]);
    MD_CHECK(ctx != NULL);
    MD_CHECK(md_mcp_server_set_write_fn(server, md_mcp_stdio_write, ctx) == 0);
    md_test_mcp_handshake(server);
    int calls = 0;
    MdMcpTool tool = { .name = "count", .handler = count_dispatch, .userdata = &calls };
    MD_CHECK(md_mcp_server_register_tool(server, &tool) == 0);
    close(output[0]);
    int64_t start = md_now_ms();
    MD_CHECK(md_mcp_stdio_write("{}", 2, ctx) == -1);
    MD_CHECK(md_now_ms() - start < 1000);

    /* Failed responses must not stop input dispatch: both tool calls run. */
    const char requests[] =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"count\"}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"count\"}}\n";
    MD_CHECK(write(input[1], requests, sizeof(requests) - 1) == sizeof(requests) - 1);
    close(input[1]);
    MD_CHECK(md_mcp_stdio_run(ctx) == 0);
    MD_CHECK(md_mcp_server_is_initialized(server));
    MD_CHECK(calls == 2);
    md_mcp_stdio_destroy(ctx);
    md_mcp_server_destroy(server);
    close(input[0]);
    close(output[1]);
    PASS("closed stdout pipe returns promptly without SIGPIPE; input dispatch survives");
}

static void test_stdio_stalled_pipe(void)
{
    int output[2];
    MD_CHECK(pipe(output) == 0);
    int flags = fcntl(output[1], F_GETFL);
    MD_CHECK(flags >= 0);
    MdMcpServerConfig config = { .server_name = "stalled-pipe" };
    MdMcpServer *server = md_mcp_server_create(&config);
    MD_CHECK(server != NULL);
    MdMcpStdio *ctx = md_mcp_stdio_create_ex(server, -1, output[1], 200);
    MD_CHECK(ctx != NULL);
    size_t len = 1024 * 1024;
    char *json = malloc(len);
    MD_CHECK(json != NULL);
    memset(json, 'x', len);
    /* POLLOUT initially succeeds, but this message exceeds pipe capacity.
     * Chunked writes must stop at the deadline without changing fd flags. */
    int64_t start = md_now_ms();
    MD_CHECK(md_mcp_stdio_write(json, len, ctx) == -1);
    int64_t elapsed = md_now_ms() - start;
    MD_CHECK(elapsed < 1500); /* watchdog only; not a scheduling assertion */
    char buf[4096];
    MD_CHECK(read(output[0], buf, sizeof(buf)) > 0);
    start = md_now_ms();
    MD_CHECK(md_mcp_stdio_write("{}", 2, ctx) == -1);
    MD_CHECK(md_now_ms() - start < 1000); /* no append to partial frame */
    free(json);
    md_mcp_stdio_destroy(ctx);
    /* macOS can add an internal pipe flag after growing its buffer; check
     * the status flag the transport actually owns, not that internal bit. */
    MD_CHECK((fcntl(output[1], F_GETFL) & O_NONBLOCK) == (flags & O_NONBLOCK));
    md_mcp_server_destroy(server);
    close(output[0]);
    close(output[1]);
    PASS("large stdio write bounded despite initial readiness; partial frame disables output");
}

typedef struct {
    MdMcpStdio *ctx;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool started, returned;
    int rc;
} NonblockingRun;

static void *nonblocking_run(void *userdata)
{
    NonblockingRun *r = userdata;
    pthread_mutex_lock(&r->mu);
    r->started = true;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    r->rc = md_mcp_stdio_run(r->ctx);
    pthread_mutex_lock(&r->mu);
    r->returned = true;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

static void test_stdio_shared_nonblocking_input(void)
{
    int pair[2];
    MD_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int out = dup(pair[0]);
    MD_CHECK(out >= 0);
    int flags = fcntl(pair[0], F_GETFL);
    MdMcpServerConfig config = { .server_name = "shared-fds" };
    MdMcpServer *server = md_mcp_server_create(&config);
    MdMcpStdio *ctx = md_mcp_stdio_create_ex(server, pair[0], out, 200);
    MD_CHECK(ctx != NULL);
    MD_CHECK(fcntl(pair[0], F_GETFL) == flags); /* create must not mutate stdin */
    MD_CHECK(fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) == 0);
    NonblockingRun run = { .ctx = ctx, .mu = PTHREAD_MUTEX_INITIALIZER,
                          .cv = PTHREAD_COND_INITIALIZER };
    pthread_t tid;
    MD_CHECK(pthread_create(&tid, NULL, nonblocking_run, &run) == 0);
    pthread_mutex_lock(&run.mu);
    while (!run.started) pthread_cond_wait(&run.cv, &run.mu);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (!run.returned) {
        int rc = pthread_cond_timedwait(&run.cv, &run.mu, &deadline);
        if (rc == ETIMEDOUT) break;
        MD_CHECK(rc == 0);
        if (rc != 0) break;
    }
    MD_CHECK(!run.returned); /* EAGAIN is not EOF */
    pthread_mutex_unlock(&run.mu);
    const char requests[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n";
    MD_CHECK(write(pair[1], requests, sizeof(requests) - 1) == sizeof(requests) - 1);
    MD_CHECK(md_test_wait_readable(pair[1], 2000) == 1);
    char response[2048] = {0};
    MD_CHECK(read(pair[1], response, sizeof(response) - 1) > 0);
    MD_CHECK(strstr(response, "\"result\"") != NULL);
    MD_CHECK(shutdown(pair[1], SHUT_WR) == 0);
    MD_CHECK(pthread_join(tid, NULL) == 0);
    MD_CHECK(run.rc == 0 && md_mcp_server_is_initialized(server));
    md_mcp_stdio_destroy(ctx);
    MD_CHECK((fcntl(pair[0], F_GETFL) & O_NONBLOCK) != 0);
    md_mcp_server_destroy(server);
    pthread_cond_destroy(&run.cv);
    pthread_mutex_destroy(&run.mu);
    close(out); close(pair[0]); close(pair[1]);
    PASS("dup'd input/output flags unchanged; initially idle nonblocking input survives");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    MD_CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR); /* same policy as host main */
    printf("MCP stdio transport tests:\n");

    test_stdio_init_and_ping();
    test_stdio_tools_list();
    test_stdio_tool_call();
    test_stdio_broken_pipe();
    test_stdio_stalled_pipe();
    test_stdio_shared_nonblocking_input();

    printf("\nAll MCP stdio tests passed.\n");
    return md_test_report();
}
