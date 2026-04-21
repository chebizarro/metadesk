/*
 * metadesk — test_mcp_stdio.c
 * Integration test for MCP stdio transport.
 *
 * Creates a pipe-based MCP server, sends JSON-RPC messages, and
 * verifies responses come back correctly over the pipe.
 */
#include "mcp_server.h"
#include "mcp_tools.h"
#include "mcp_stdio.h"
#include "jsonrpc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define PASS(name) printf("  PASS  %s\n", name)

/* ── Pipe-based test harness ─────────────────────────────────── */

typedef struct {
    int agent_to_server[2];   /* agent writes, server reads */
    int server_to_agent[2];   /* server writes, agent reads */
    MdMcpServer *server;
    MdMcpStdio  *stdio_ctx;
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
    MdMcpToolCtx tool_ctx = { .agent = NULL, .a11y = NULL };
    md_mcp_register_tools(h->server, &tool_ctx);

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

    close(h->agent_to_server[0]);
    close(h->server_to_agent[0]);
    close(h->server_to_agent[1]);

    md_mcp_stdio_destroy(h->stdio_ctx);
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
                     "\"id\":1,\"params\":{\"protocolVersion\":\"2025-03-26\","
                     "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}");

    char *resp = harness_recv(h);
    assert(resp != NULL);
    cJSON *r = cJSON_Parse(resp);
    assert(cJSON_GetObjectItem(r, "result") != NULL);
    cJSON_Delete(r);
    free(resp);

    /* Send initialized notification */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

    /* Small delay for notification processing */
    usleep(10000);

    /* Send ping */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":2}");
    resp = harness_recv(h);
    assert(resp != NULL);
    r = cJSON_Parse(resp);
    assert(cJSON_GetObjectItem(r, "result") != NULL);
    assert(cJSON_GetObjectItem(r, "id")->valueint == 2);
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
    usleep(10000);

    /* List tools */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":2}");
    char *resp = harness_recv(h);
    assert(resp != NULL);

    cJSON *r = cJSON_Parse(resp);
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "result"), "tools");
    assert(cJSON_GetArraySize(tools) == 9);
    cJSON_Delete(r);
    free(resp);

    harness_destroy(h);
    PASS("stdio tools/list returns 9 tools");
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
    usleep(10000);

    /* Call click tool */
    harness_send(h, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":3,"
                     "\"params\":{\"name\":\"metadesk_click\","
                     "\"arguments\":{\"target_id\":\"n42\"}}}");
    char *resp = harness_recv(h);
    assert(resp != NULL);

    cJSON *r = cJSON_Parse(resp);
    cJSON *result = cJSON_GetObjectItem(r, "result");
    cJSON *content = cJSON_GetObjectItem(result, "content");
    assert(cJSON_IsArray(content));
    assert(cJSON_GetArraySize(content) >= 1);
    cJSON_Delete(r);
    free(resp);

    harness_destroy(h);
    PASS("stdio tool call round-trip");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("MCP stdio transport tests:\n");

    test_stdio_init_and_ping();
    test_stdio_tools_list();
    test_stdio_tool_call();

    printf("\nAll MCP stdio tests passed.\n");
    return 0;
}
