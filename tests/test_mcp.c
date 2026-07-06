/*
 * metadesk — test_mcp.c
 * Unit tests for MCP server core.
 */
#include "mcp_server.h"
#include "mcp_tools.h"
#include "mcp_resources.h"
#include "mcp_bridge.h"
#include "jsonrpc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASS(name) printf("  PASS  %s\n", name)

/* ── Capture transport ───────────────────────────────────────── */

#define MAX_RESPONSES 32
static char *g_responses[MAX_RESPONSES];
static int   g_response_count = 0;

static void clear_responses(void)
{
    for (int i = 0; i < g_response_count; i++) {
        free(g_responses[i]);
        g_responses[i] = NULL;
    }
    g_response_count = 0;
}

static int capture_write(const char *json, size_t len, void *userdata)
{
    (void)userdata;
    if (g_response_count >= MAX_RESPONSES) return -1;
    g_responses[g_response_count] = strndup(json, len);
    g_response_count++;
    return 0;
}

static cJSON *last_response(void)
{
    assert(g_response_count > 0);
    return cJSON_Parse(g_responses[g_response_count - 1]);
}

/* ── Stub tool handler ───────────────────────────────────────── */

static cJSON *stub_tool_handler(const cJSON *arguments,
                                bool *is_error, char **error_msg,
                                void *userdata)
{
    (void)is_error;
    (void)error_msg;
    (void)userdata;

    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");

    /* Echo back a summary */
    const char *target = NULL;
    if (arguments) {
        cJSON *tid = cJSON_GetObjectItemCaseSensitive(arguments, "target_id");
        if (cJSON_IsString(tid)) target = tid->valuestring;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "clicked %s", target ? target : "nothing");
    cJSON_AddStringToObject(item, "text", buf);
    cJSON_AddItemToArray(content, item);

    return content;
}

/* Stub resource handler */
static cJSON *stub_resource_read(void *userdata)
{
    (void)userdata;
    cJSON *contents = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "uri", "metadesk://ui-tree");
    cJSON_AddStringToObject(item, "mimeType", "application/json");
    cJSON_AddStringToObject(item, "text", "{\"root\":{\"id\":\"n1\"}}");
    cJSON_AddItemToArray(contents, item);
    return contents;
}

/* ── Helpers ─────────────────────────────────────────────────── */

static MdMcpServer *make_server(void)
{
    clear_responses();
    MdMcpServerConfig cfg = {
        .server_name = "metadesk-test",
        .server_version = "0.0.1",
        .write_fn = capture_write,
    };
    return md_mcp_server_create(&cfg);
}

static void do_init(MdMcpServer *s)
{
    const char *init = "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
                       "\"id\":1,\"params\":{\"protocolVersion\":\"2025-03-26\","
                       "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}";
    md_mcp_server_handle_message(s, init, strlen(init));

    const char *initialized = "{\"jsonrpc\":\"2.0\","
                              "\"method\":\"notifications/initialized\"}";
    md_mcp_server_handle_message(s, initialized, strlen(initialized));
}

/* ── Tests ───────────────────────────────────────────────────── */

static void test_initialize(void)
{
    MdMcpServer *s = make_server();

    const char *init = "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
                       "\"id\":\"init-1\","
                       "\"params\":{\"protocolVersion\":\"2025-03-26\","
                       "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}";
    md_mcp_server_handle_message(s, init, strlen(init));

    assert(g_response_count == 1);
    cJSON *resp = last_response();
    assert(resp != NULL);

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    assert(result != NULL);
    assert(strcmp(cJSON_GetObjectItem(result, "protocolVersion")->valuestring,
                 "2025-03-26") == 0);

    cJSON *info = cJSON_GetObjectItem(result, "serverInfo");
    assert(strcmp(cJSON_GetObjectItem(info, "name")->valuestring,
                 "metadesk-test") == 0);

    cJSON_Delete(resp);
    md_mcp_server_destroy(s);
    PASS("initialize handshake");
}

static void test_reject_before_init(void)
{
    MdMcpServer *s = make_server();

    const char *ping = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":1}";
    md_mcp_server_handle_message(s, ping, strlen(ping));

    cJSON *resp = last_response();
    assert(cJSON_GetObjectItem(resp, "error") != NULL);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("reject calls before initialize");
}

static void test_ping(void)
{
    MdMcpServer *s = make_server();
    do_init(s);
    clear_responses();

    const char *ping = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":99}";
    md_mcp_server_handle_message(s, ping, strlen(ping));

    cJSON *resp = last_response();
    assert(cJSON_GetObjectItem(resp, "result") != NULL);
    assert(cJSON_GetObjectItem(resp, "id")->valueint == 99);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("ping");
}

static void test_tools_list(void)
{
    MdMcpServer *s = make_server();

    /* Register a tool */
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    MdMcpTool tool = {
        .name = "metadesk_click",
        .description = "Click a UI element",
        .input_schema = schema,
        .handler = stub_tool_handler,
    };
    md_mcp_server_register_tool(s, &tool);

    do_init(s);
    clear_responses();

    const char *list = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":2}";
    md_mcp_server_handle_message(s, list, strlen(list));

    cJSON *resp = last_response();
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *tools = cJSON_GetObjectItem(result, "tools");
    assert(cJSON_IsArray(tools));
    assert(cJSON_GetArraySize(tools) == 1);

    cJSON *t = cJSON_GetArrayItem(tools, 0);
    assert(strcmp(cJSON_GetObjectItem(t, "name")->valuestring,
                 "metadesk_click") == 0);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("tools/list");
}

static void test_tools_call(void)
{
    MdMcpServer *s = make_server();

    cJSON *schema = cJSON_CreateObject();
    MdMcpTool tool = {
        .name = "metadesk_click",
        .description = "Click",
        .input_schema = schema,
        .handler = stub_tool_handler,
    };
    md_mcp_server_register_tool(s, &tool);
    do_init(s);
    clear_responses();

    const char *call = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":3,"
                       "\"params\":{\"name\":\"metadesk_click\","
                       "\"arguments\":{\"target_id\":\"btn_ok\"}}}";
    md_mcp_server_handle_message(s, call, strlen(call));

    cJSON *resp = last_response();
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *content = cJSON_GetObjectItem(result, "content");
    assert(cJSON_IsArray(content));
    assert(cJSON_GetArraySize(content) == 1);

    cJSON *item = cJSON_GetArrayItem(content, 0);
    assert(strcmp(cJSON_GetObjectItem(item, "text")->valuestring,
                 "clicked btn_ok") == 0);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("tools/call");
}

static void test_tools_call_unknown(void)
{
    MdMcpServer *s = make_server();
    do_init(s);
    clear_responses();

    const char *call = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":4,"
                       "\"params\":{\"name\":\"nonexistent\"}}";
    md_mcp_server_handle_message(s, call, strlen(call));

    cJSON *resp = last_response();
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    assert(err != NULL);
    assert(cJSON_GetObjectItem(err, "code")->valueint == -32601);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("tools/call unknown tool");
}

static void test_resources_list(void)
{
    MdMcpServer *s = make_server();

    MdMcpResource res = {
        .uri = "metadesk://ui-tree",
        .name = "UI Tree",
        .description = "Current accessibility tree",
        .mime_type = "application/json",
        .read_handler = stub_resource_read,
    };
    md_mcp_server_register_resource(s, &res);
    do_init(s);
    clear_responses();

    const char *list = "{\"jsonrpc\":\"2.0\",\"method\":\"resources/list\","
                       "\"id\":5}";
    md_mcp_server_handle_message(s, list, strlen(list));

    cJSON *resp = last_response();
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *resources = cJSON_GetObjectItem(result, "resources");
    assert(cJSON_IsArray(resources));
    assert(cJSON_GetArraySize(resources) == 1);

    cJSON *r = cJSON_GetArrayItem(resources, 0);
    assert(strcmp(cJSON_GetObjectItem(r, "uri")->valuestring,
                 "metadesk://ui-tree") == 0);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("resources/list");
}

static void test_resources_read(void)
{
    MdMcpServer *s = make_server();

    MdMcpResource res = {
        .uri = "metadesk://ui-tree",
        .name = "UI Tree",
        .mime_type = "application/json",
        .read_handler = stub_resource_read,
    };
    md_mcp_server_register_resource(s, &res);
    do_init(s);
    clear_responses();

    const char *read = "{\"jsonrpc\":\"2.0\",\"method\":\"resources/read\","
                       "\"id\":6,\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    md_mcp_server_handle_message(s, read, strlen(read));

    cJSON *resp = last_response();
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *contents = cJSON_GetObjectItem(result, "contents");
    assert(cJSON_IsArray(contents));
    assert(cJSON_GetArraySize(contents) == 1);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("resources/read");
}

static void test_resources_subscribe(void)
{
    MdMcpServer *s = make_server();

    MdMcpResource res = {
        .uri = "metadesk://ui-tree",
        .name = "UI Tree",
        .mime_type = "application/json",
        .read_handler = stub_resource_read,
    };
    md_mcp_server_register_resource(s, &res);
    do_init(s);
    clear_responses();

    /* Subscribe */
    const char *sub = "{\"jsonrpc\":\"2.0\",\"method\":\"resources/subscribe\","
                      "\"id\":7,\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    md_mcp_server_handle_message(s, sub, strlen(sub));
    assert(g_response_count == 1);

    /* Now a notification should be sent */
    clear_responses();
    md_mcp_server_notify_resource_updated(s, "metadesk://ui-tree");
    assert(g_response_count == 1);

    cJSON *notif = cJSON_Parse(g_responses[0]);
    assert(strcmp(cJSON_GetObjectItem(notif, "method")->valuestring,
                 "notifications/resources/updated") == 0);
    assert(!cJSON_HasObjectItem(notif, "id"));
    cJSON_Delete(notif);

    /* Unsubscribe */
    clear_responses();
    const char *unsub = "{\"jsonrpc\":\"2.0\",\"method\":\"resources/unsubscribe\","
                        "\"id\":8,\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    md_mcp_server_handle_message(s, unsub, strlen(unsub));

    /* Notification should not be sent */
    clear_responses();
    md_mcp_server_notify_resource_updated(s, "metadesk://ui-tree");
    assert(g_response_count == 0);

    md_mcp_server_destroy(s);
    PASS("resources/subscribe + unsubscribe");
}

static void test_unknown_method(void)
{
    MdMcpServer *s = make_server();
    do_init(s);
    clear_responses();

    const char *msg = "{\"jsonrpc\":\"2.0\",\"method\":\"foo/bar\",\"id\":10}";
    md_mcp_server_handle_message(s, msg, strlen(msg));

    cJSON *resp = last_response();
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    assert(err != NULL);
    assert(cJSON_GetObjectItem(err, "code")->valueint == -32601);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("unknown method → -32601");
}

static void test_parse_error(void)
{
    MdMcpServer *s = make_server();

    md_mcp_server_handle_message(s, "{{bad json", 10);

    cJSON *resp = last_response();
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    assert(cJSON_GetObjectItem(err, "code")->valueint == -32700);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("parse error → -32700");
}

static void test_init_capabilities_reflect_registrations(void)
{
    MdMcpServer *s = make_server();

    /* Register a tool and resource before init */
    cJSON *schema = cJSON_CreateObject();
    MdMcpTool tool = {
        .name = "test_tool", .input_schema = schema,
        .handler = stub_tool_handler,
    };
    md_mcp_server_register_tool(s, &tool);

    MdMcpResource res = {
        .uri = "test://res", .name = "Test",
        .read_handler = stub_resource_read,
    };
    md_mcp_server_register_resource(s, &res);

    clear_responses();
    const char *init = "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
                       "\"id\":1,\"params\":{}}";
    md_mcp_server_handle_message(s, init, strlen(init));

    cJSON *resp = last_response();
    cJSON *caps = cJSON_GetObjectItem(
        cJSON_GetObjectItem(resp, "result"), "capabilities");
    assert(cJSON_HasObjectItem(caps, "tools"));
    assert(cJSON_HasObjectItem(caps, "resources"));
    cJSON *res_caps = cJSON_GetObjectItem(caps, "resources");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(res_caps, "subscribe")));
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("init capabilities reflect registrations");
}

/* ── Bridge lifecycle test ────────────────────────────────── */

static void test_bridge_create_destroy(void)
{
    /* Create a bridge with pipe transport */
    int in_pipe[2], out_pipe[2];
    pipe(in_pipe);
    pipe(out_pipe);

    MdMcpBridgeConfig cfg = {
        .a11y = NULL,
        .input = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms = 50,
        .stdio_in_fd = in_pipe[0],
        .stdio_out_fd = out_pipe[1],
    };

    MdMcpBridge *bridge = md_mcp_bridge_create(&cfg);
    assert(bridge != NULL);

    /* Session should be active */
    assert(md_mcp_bridge_get_state(bridge) == MD_SESSION_ACTIVE);

    /* Server should be accessible */
    assert(md_mcp_bridge_get_server(bridge) != NULL);

    /* Clean destroy */
    md_mcp_bridge_destroy(bridge);

    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);

    PASS("bridge create + destroy lifecycle");
}

/* ── Resource registration tests ─────────────────────────────── */

static void test_resources_with_session(void)
{
    MdMcpServer *s = make_server();

    /* Set up a mock session */
    MdSession session;
    md_session_init(&session);
    strncpy(session.session_id, "test-uuid-123", sizeof(session.session_id));
    strncpy(session.peer_npub, "npub1test", sizeof(session.peer_npub));
    session.state = MD_SESSION_ACTIVE;
    session.capabilities = MD_CAP_AGENT | MD_CAP_INPUT;

    MdMcpResourceCtx res_ctx = {
        .a11y = NULL,  /* no real a11y in test */
        .session = &session,
        .agent = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
    };
    assert(md_mcp_register_resources(s, &res_ctx) == 0);
    do_init(s);
    clear_responses();

    /* Read session info */
    const char *read = "{\"jsonrpc\":\"2.0\",\"method\":\"resources/read\","
                       "\"id\":1,\"params\":{\"uri\":\"metadesk://session-info\"}}";
    md_mcp_server_handle_message(s, read, strlen(read));

    cJSON *resp = last_response();
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *contents = cJSON_GetObjectItem(result, "contents");
    assert(cJSON_IsArray(contents));
    assert(cJSON_GetArraySize(contents) == 1);

    cJSON *item = cJSON_GetArrayItem(contents, 0);
    const char *text = cJSON_GetObjectItem(item, "text")->valuestring;
    cJSON *info = cJSON_Parse(text);
    assert(info != NULL);
    assert(strcmp(cJSON_GetObjectItem(info, "session_id")->valuestring,
                 "test-uuid-123") == 0);
    assert(strcmp(cJSON_GetObjectItem(info, "state")->valuestring,
                 "active") == 0);
    cJSON *caps = cJSON_GetObjectItem(info, "capabilities");
    assert(cJSON_GetArraySize(caps) == 2);
    cJSON_Delete(info);
    cJSON_Delete(resp);

    /* Read UI tree (no a11y → returns null root) */
    clear_responses();
    const char *read_tree = "{\"jsonrpc\":\"2.0\",\"method\":\"resources/read\","
                            "\"id\":2,\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    md_mcp_server_handle_message(s, read_tree, strlen(read_tree));

    resp = last_response();
    result = cJSON_GetObjectItem(resp, "result");
    contents = cJSON_GetObjectItem(result, "contents");
    assert(cJSON_GetArraySize(contents) == 1);
    item = cJSON_GetArrayItem(contents, 0);
    assert(strcmp(cJSON_GetObjectItem(item, "uri")->valuestring,
                 "metadesk://ui-tree") == 0);
    cJSON_Delete(resp);

    /* resources/list should show 2 resources */
    clear_responses();
    const char *list = "{\"jsonrpc\":\"2.0\",\"method\":\"resources/list\","
                       "\"id\":3}";
    md_mcp_server_handle_message(s, list, strlen(list));
    resp = last_response();
    cJSON *resources = cJSON_GetObjectItem(
        cJSON_GetObjectItem(resp, "result"), "resources");
    assert(cJSON_GetArraySize(resources) == 2);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("resources: session-info + ui-tree with mock session");
}

/* ── Tool registration tests ──────────────────────────────────── */

static void test_9_tools_register(void)
{
    MdMcpServer *s = make_server();
    MdMcpToolCtx tool_ctx = { .agent = NULL, .a11y = NULL };
    assert(md_mcp_register_tools(s, &tool_ctx) == 0);

    do_init(s);
    clear_responses();

    const char *list = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":1}";
    md_mcp_server_handle_message(s, list, strlen(list));

    cJSON *resp = last_response();
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "tools");
    assert(cJSON_GetArraySize(tools) == 9);

    /* Verify known tool names */
    const char *expected[] = {
        "metadesk_click", "metadesk_dbl_click", "metadesk_right_click",
        "metadesk_type", "metadesk_key_combo", "metadesk_scroll",
        "metadesk_focus", "metadesk_set_value", "metadesk_screenshot"
    };
    for (int i = 0; i < 9; i++) {
        cJSON *t = cJSON_GetArrayItem(tools, i);
        assert(strcmp(cJSON_GetObjectItem(t, "name")->valuestring,
                     expected[i]) == 0);
        assert(cJSON_HasObjectItem(t, "inputSchema"));
    }
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("9 tools register with schemas");
}

static void test_tool_call_click_no_agent(void)
{
    MdMcpServer *s = make_server();
    MdMcpToolCtx tool_ctx = { .agent = NULL, .a11y = NULL };
    md_mcp_register_tools(s, &tool_ctx);
    do_init(s);
    clear_responses();

    const char *call = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":2,"
                       "\"params\":{\"name\":\"metadesk_click\","
                       "\"arguments\":{\"target_id\":\"btn_ok\"}}}";
    md_mcp_server_handle_message(s, call, strlen(call));

    cJSON *resp = last_response();
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    /* No agent → isError=true with descriptive message */
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
    cJSON *content = cJSON_GetObjectItem(result, "content");
    assert(cJSON_IsArray(content));
    assert(cJSON_GetArraySize(content) == 1);
    cJSON *text_item = cJSON_GetArrayItem(content, 0);
    const char *text = cJSON_GetObjectItem(text_item, "text")->valuestring;
    assert(strstr(text, "no agent") != NULL);
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("tool call click (no agent) returns isError");
}

static void test_tool_call_missing_target(void)
{
    MdMcpServer *s = make_server();
    MdMcpToolCtx tool_ctx = { .agent = NULL, .a11y = NULL };
    md_mcp_register_tools(s, &tool_ctx);
    do_init(s);
    clear_responses();

    /* Click with no target_id should error */
    const char *call = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":3,"
                       "\"params\":{\"name\":\"metadesk_click\","
                       "\"arguments\":{}}}";
    md_mcp_server_handle_message(s, call, strlen(call));

    cJSON *resp = last_response();
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("tool call missing target_id → isError");
}

static void test_tool_call_key_combo(void)
{
    MdMcpServer *s = make_server();
    MdMcpToolCtx tool_ctx = { .agent = NULL, .a11y = NULL };
    md_mcp_register_tools(s, &tool_ctx);
    do_init(s);
    clear_responses();

    const char *call = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":4,"
                       "\"params\":{\"name\":\"metadesk_key_combo\","
                       "\"arguments\":{\"keys\":[\"ctrl\",\"s\"]}}}";
    md_mcp_server_handle_message(s, call, strlen(call));

    cJSON *resp = last_response();
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    /* No agent → isError=true */
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
    cJSON *content = cJSON_GetObjectItem(result, "content");
    const char *text = cJSON_GetObjectItem(cJSON_GetArrayItem(content, 0), "text")->valuestring;
    assert(strstr(text, "no agent") != NULL);
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("tool call key_combo (no agent) returns isError");
}

static void test_tool_registration_failure_rolls_back(void)
{
    MdMcpServer *s = make_server();
    char names[24][32];

    /* Leave only 8 slots free so registering the 9 built-in tools fails
     * after several have already been accepted by the server. */
    for (int i = 0; i < 24; i++) {
        snprintf(names[i], sizeof(names[i]), "prefill_tool_%02d", i);
        cJSON *schema = cJSON_CreateObject();
        MdMcpTool tool = {
            .name = names[i],
            .input_schema = schema,
            .handler = stub_tool_handler,
        };
        assert(md_mcp_server_register_tool(s, &tool) == 0);
    }

    MdMcpToolCtx tool_ctx = { .agent = NULL, .a11y = NULL };
    assert(md_mcp_register_tools(s, &tool_ctx) == -1);
    assert(tool_ctx._handler_ctxs == NULL);

    do_init(s);
    clear_responses();
    const char *list = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":9}";
    md_mcp_server_handle_message(s, list, strlen(list));

    cJSON *resp = last_response();
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "tools");
    assert(cJSON_GetArraySize(tools) == 24);
    for (int i = 0; i < cJSON_GetArraySize(tools); i++) {
        cJSON *t = cJSON_GetArrayItem(tools, i);
        assert(strncmp(cJSON_GetObjectItem(t, "name")->valuestring,
                       "prefill_tool_", 13) == 0);
    }
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("tool registration failure rolls back partial tools");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("MCP server core tests:\n");

    test_initialize();
    test_reject_before_init();
    test_ping();
    test_tools_list();
    test_tools_call();
    test_tools_call_unknown();
    test_resources_list();
    test_resources_read();
    test_resources_subscribe();
    test_unknown_method();
    test_parse_error();
    test_init_capabilities_reflect_registrations();
    test_9_tools_register();
    test_tool_call_click_no_agent();
    test_tool_call_missing_target();
    test_tool_call_key_combo();
    test_tool_registration_failure_rolls_back();
    test_resources_with_session();
    test_bridge_create_destroy();

    printf("\nAll MCP server tests passed.\n");
    clear_responses();
    return 0;
}
