/*
 * metadesk — test_mcp.c
 * Unit tests for MCP server core.
 */
#include "mcp_server.h"
#include "md_mcp_fixture.h"
#include "mcp_tools.h"
#include "mcp_resources.h"
#include "mcp_bridge.h"
#include "jsonrpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include "md_test.h"


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
    MD_CHECK(g_response_count > 0);
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

#define MD_TEST_MCP_PROTOCOL "2025-03-26"

static void do_init(MdMcpServer *s)
{
    md_test_mcp_handshake(s);
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

    MD_CHECK(g_response_count == 1);
    cJSON *resp = last_response();
    MD_CHECK(resp != NULL);

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    MD_CHECK(result != NULL);
    MD_CHECK(strcmp(cJSON_GetObjectItem(result, "protocolVersion")->valuestring,
                 MD_TEST_MCP_PROTOCOL) == 0);

    cJSON *info = cJSON_GetObjectItem(result, "serverInfo");
    MD_CHECK(strcmp(cJSON_GetObjectItem(info, "name")->valuestring,
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
    MD_CHECK(cJSON_GetObjectItem(resp, "error") != NULL);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("reject calls before initialize");
}

static void test_initialized_before_initialize_ignored(void)
{
    MdMcpServer *s = make_server();

    const char *initialized = "{\"jsonrpc\":\"2.0\","
                              "\"method\":\"notifications/initialized\"}";
    md_mcp_server_handle_message(s, initialized, strlen(initialized));
    MD_CHECK(!md_mcp_server_is_initialized(s));
    MD_CHECK(g_response_count == 0);

    const char *ping = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":1}";
    md_mcp_server_handle_message(s, ping, strlen(ping));

    cJSON *resp = last_response();
    MD_CHECK(cJSON_GetObjectItem(resp, "error") != NULL);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("initialized notification before initialize is ignored");
}

static void test_ping(void)
{
    MdMcpServer *s = make_server();
    do_init(s);
    clear_responses();

    const char *ping = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":99}";
    md_mcp_server_handle_message(s, ping, strlen(ping));

    cJSON *resp = last_response();
    MD_CHECK(cJSON_GetObjectItem(resp, "result") != NULL);
    MD_CHECK(cJSON_GetObjectItem(resp, "id")->valueint == 99);
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
    MD_CHECK(cJSON_IsArray(tools));
    MD_CHECK(cJSON_GetArraySize(tools) == 1);

    cJSON *t = cJSON_GetArrayItem(tools, 0);
    MD_CHECK(strcmp(cJSON_GetObjectItem(t, "name")->valuestring,
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
    MD_CHECK(cJSON_IsArray(content));
    MD_CHECK(cJSON_GetArraySize(content) == 1);

    cJSON *item = cJSON_GetArrayItem(content, 0);
    MD_CHECK(strcmp(cJSON_GetObjectItem(item, "text")->valuestring,
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
    MD_CHECK(err != NULL);
    /* Named-thing-not-registered answers INVALID_PARAMS, same as
     * resources/read — the method itself exists. */
    MD_CHECK(cJSON_GetObjectItem(err, "code")->valueint == MD_JSONRPC_INVALID_PARAMS);
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
    MD_CHECK(cJSON_IsArray(resources));
    MD_CHECK(cJSON_GetArraySize(resources) == 1);

    cJSON *r = cJSON_GetArrayItem(resources, 0);
    MD_CHECK(strcmp(cJSON_GetObjectItem(r, "uri")->valuestring,
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
    MD_CHECK(cJSON_IsArray(contents));
    MD_CHECK(cJSON_GetArraySize(contents) == 1);
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
    MD_CHECK(g_response_count == 1);

    /* Now a notification should be sent */
    clear_responses();
    md_mcp_server_notify_resource_updated(s, "metadesk://ui-tree");
    MD_CHECK(g_response_count == 1);

    cJSON *notif = cJSON_Parse(g_responses[0]);
    MD_CHECK(strcmp(cJSON_GetObjectItem(notif, "method")->valuestring,
                 "notifications/resources/updated") == 0);
    MD_CHECK(!cJSON_HasObjectItem(notif, "id"));
    cJSON_Delete(notif);

    /* Unsubscribe */
    clear_responses();
    const char *unsub = "{\"jsonrpc\":\"2.0\",\"method\":\"resources/unsubscribe\","
                        "\"id\":8,\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    md_mcp_server_handle_message(s, unsub, strlen(unsub));

    /* Notification should not be sent */
    clear_responses();
    md_mcp_server_notify_resource_updated(s, "metadesk://ui-tree");
    MD_CHECK(g_response_count == 0);

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
    MD_CHECK(err != NULL);
    MD_CHECK(cJSON_GetObjectItem(err, "code")->valueint == -32601);
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
    MD_CHECK(cJSON_GetObjectItem(err, "code")->valueint == -32700);
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
    MD_CHECK(cJSON_HasObjectItem(caps, "tools"));
    MD_CHECK(cJSON_HasObjectItem(caps, "resources"));
    cJSON *res_caps = cJSON_GetObjectItem(caps, "resources");
    MD_CHECK(cJSON_IsTrue(cJSON_GetObjectItem(res_caps, "subscribe")));
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("init capabilities reflect registrations");
}

/* ── Bridge lifecycle test ────────────────────────────────── */

static void test_bridge_null_a11y_fails_create(void)
{
    MdMcpBridgeConfig cfg = {
        .a11y = NULL,
        .input = NULL,
        .tree_format = MD_MCP_TREE_FORMAT_JSON,
        .settle_ms = 50,
        .stdio_in_fd = -1,
        .stdio_out_fd = -1,
    };

    /* a11y is required — no silent degraded mode */
    MD_CHECK(md_mcp_bridge_create(&cfg) == NULL);
    MD_CHECK(md_mcp_bridge_create(NULL) == NULL);

    PASS("bridge NULL a11y fails create");
}

static void test_bridge_create_destroy_with_stub_a11y(void)
{
    /* Minimal a11y stub: every vtable call fails safely. */
    static const MdA11yBackend stub_vtable = {0};
    MdA11yCtx stub_a11y = { .vtable = &stub_vtable };

    /* Create a bridge with pipe transport and missing production deps. */
    int in_pipe[2], out_pipe[2];
    pipe(in_pipe);
    pipe(out_pipe);

    MdMcpBridgeConfig cfg = {
        .a11y = &stub_a11y,
        .input = NULL,
        .tree_format = MD_MCP_TREE_FORMAT_JSON,
        .settle_ms = 50,
        .stdio_in_fd = in_pipe[0],
        .stdio_out_fd = out_pipe[1],
    };

    MdMcpBridge *bridge = md_mcp_bridge_create(&cfg);
    MD_CHECK(bridge != NULL);

    /* Session should be active. */
    MD_CHECK(md_mcp_bridge_get_state(bridge) == MD_SESSION_ACTIVE);

    /* Server should be accessible */
    MD_CHECK(md_mcp_bridge_get_server(bridge) != NULL);

    /* Clean destroy */
    md_mcp_bridge_destroy(bridge);

    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);

    PASS("bridge create + destroy lifecycle with stub a11y");
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
        .snapshot = NULL,  /* no real a11y in test */
        .session = &session,
        .agent = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
    };
    MD_CHECK(md_mcp_register_resources(s, &res_ctx) == 0);
    do_init(s);
    clear_responses();

    /* Read session info */
    const char *read = "{\"jsonrpc\":\"2.0\",\"method\":\"resources/read\","
                       "\"id\":1,\"params\":{\"uri\":\"metadesk://session-info\"}}";
    md_mcp_server_handle_message(s, read, strlen(read));

    cJSON *resp = last_response();
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *contents = cJSON_GetObjectItem(result, "contents");
    MD_CHECK(cJSON_IsArray(contents));
    MD_CHECK(cJSON_GetArraySize(contents) == 1);

    cJSON *item = cJSON_GetArrayItem(contents, 0);
    const char *text = cJSON_GetObjectItem(item, "text")->valuestring;
    cJSON *info = cJSON_Parse(text);
    MD_CHECK(info != NULL);
    MD_CHECK(strcmp(cJSON_GetObjectItem(info, "session_id")->valuestring,
                 "test-uuid-123") == 0);
    MD_CHECK(strcmp(cJSON_GetObjectItem(info, "state")->valuestring,
                 "active") == 0);
    cJSON *caps = cJSON_GetObjectItem(info, "capabilities");
    MD_CHECK(cJSON_GetArraySize(caps) == 2);
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
    MD_CHECK(cJSON_GetArraySize(contents) == 1);
    item = cJSON_GetArrayItem(contents, 0);
    MD_CHECK(strcmp(cJSON_GetObjectItem(item, "uri")->valuestring,
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
    MD_CHECK(cJSON_GetArraySize(resources) == 2);
    cJSON_Delete(resp);

    md_mcp_server_destroy(s);
    PASS("resources: session-info + ui-tree with mock session");
}

/* ── Tool registration tests ──────────────────────────────────── */

static void test_14_tools_register(void)
{
    MdMcpServer *s = make_server();
    MdMcpToolCtx tool_ctx = { .agent = NULL };
    MD_CHECK(md_mcp_register_tools(s, &tool_ctx) == 0);

    do_init(s);
    clear_responses();

    const char *list = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":1}";
    md_mcp_server_handle_message(s, list, strlen(list));

    cJSON *resp = last_response();
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "tools");
    MD_CHECK(cJSON_GetArraySize(tools) == 14);

    /* Verify known tool names */
    const char *expected[] = {
        "metadesk_click", "metadesk_dbl_click", "metadesk_right_click",
        "metadesk_type", "metadesk_key_combo", "metadesk_scroll",
        "metadesk_focus", "metadesk_set_value", "metadesk_screenshot",
        "metadesk_find", "metadesk_subtree", "metadesk_node",
        "metadesk_focused", "metadesk_windows"
    };
    for (int i = 0; i < 14; i++) {
        cJSON *t = cJSON_GetArrayItem(tools, i);
        MD_CHECK(strcmp(cJSON_GetObjectItem(t, "name")->valuestring,
                     expected[i]) == 0);
        MD_CHECK(cJSON_HasObjectItem(t, "inputSchema"));
    }
    cJSON *find_schema = cJSON_GetObjectItem(cJSON_GetArrayItem(tools, 9), "inputSchema");
    cJSON *props = cJSON_GetObjectItem(find_schema, "properties");
    cJSON *format = cJSON_GetObjectItem(props, "format");
    MD_CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(format, "enum")) == 2);
    MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(format, "default")), "compact") == 0);
    MD_CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(props, "limit"), "default")->valueint == 50);
    MD_CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(props, "limit"), "maximum")->valueint == 500);
    MD_CHECK(cJSON_IsFalse(cJSON_GetObjectItem(find_schema, "additionalProperties")));
    cJSON *subtree_schema = cJSON_GetObjectItem(cJSON_GetArrayItem(tools, 10), "inputSchema");
    MD_CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(subtree_schema, "required")) == 1);
    props = cJSON_GetObjectItem(subtree_schema, "properties");
    MD_CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(props, "depth"), "default")->valueint == 2);
    MD_CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(props, "depth"), "maximum")->valueint == 64);
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("14 tools register with schemas");
}

static void test_tool_call_click_no_agent(void)
{
    MdMcpServer *s = make_server();
    MdMcpToolCtx tool_ctx = { .agent = NULL };
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
    MD_CHECK(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
    cJSON *content = cJSON_GetObjectItem(result, "content");
    MD_CHECK(cJSON_IsArray(content));
    MD_CHECK(cJSON_GetArraySize(content) == 1);
    cJSON *text_item = cJSON_GetArrayItem(content, 0);
    const char *text = cJSON_GetObjectItem(text_item, "text")->valuestring;
    MD_CHECK(strstr(text, "no agent") != NULL);
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("tool call click (no agent) returns isError");
}

static void test_tool_call_missing_target(void)
{
    MdMcpServer *s = make_server();
    MdMcpToolCtx tool_ctx = { .agent = NULL };
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
    MD_CHECK(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("tool call missing target_id → isError");
}

static void test_tool_call_key_combo(void)
{
    MdMcpServer *s = make_server();
    MdMcpToolCtx tool_ctx = { .agent = NULL };
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
    MD_CHECK(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
    cJSON *content = cJSON_GetObjectItem(result, "content");
    const char *text = cJSON_GetObjectItem(cJSON_GetArrayItem(content, 0), "text")->valuestring;
    MD_CHECK(strstr(text, "no agent") != NULL);
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("tool call key_combo (no agent) returns isError");
}

static void test_tool_registration_failure_rolls_back(int prefill)
{
    MdMcpServer *s = make_server();
    char names[24][32];

    /* Exercise failure within both the action and read registrations. */
    for (int i = 0; i < prefill; i++) {
        snprintf(names[i], sizeof(names[i]), "prefill_tool_%02d", i);
        cJSON *schema = cJSON_CreateObject();
        MdMcpTool tool = {
            .name = names[i],
            .input_schema = schema,
            .handler = stub_tool_handler,
        };
        MD_CHECK(md_mcp_server_register_tool(s, &tool) == 0);
    }

    MdMcpToolCtx tool_ctx = { .agent = NULL };
    MD_CHECK(md_mcp_register_tools(s, &tool_ctx) == -1);
    MD_CHECK(tool_ctx._handler_ctxs == NULL);

    do_init(s);
    clear_responses();
    const char *list = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":9}";
    md_mcp_server_handle_message(s, list, strlen(list));

    cJSON *resp = last_response();
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "tools");
    MD_CHECK(cJSON_GetArraySize(tools) == prefill);
    for (int i = 0; i < cJSON_GetArraySize(tools); i++) {
        cJSON *t = cJSON_GetArrayItem(tools, i);
        MD_CHECK(strncmp(cJSON_GetObjectItem(t, "name")->valuestring,
                       "prefill_tool_", 13) == 0);
    }
    cJSON_Delete(resp);

    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(s);
    PASS("tool registration failure rolls back partial tools");
}

/* ── Targeted accessibility reads ───────────────────────────── */

typedef struct {
    int walks;
    bool unavailable, no_focus, many_buttons, bare_focus, unusual_nodes;
    const char *modal_role;
    MdA11yChangeCb change_cb;
    void *change_userdata;
    bool subscribe_fails;
    int subscribes, unsubscribes;
    MdA11ySnapshot *invalidate_during_walk;
} QueryFixture;

/* Stack-built MdA11yCtx fixtures need the mutexes md_a11y_create() sets up. */
static void query_a11y_init(MdA11yCtx *a11y)
{
    MD_CHECK(pthread_mutex_init(&a11y->snapshot_mu, NULL) == 0);
    MD_CHECK(pthread_mutex_init(&a11y->change_mu, NULL) == 0);
    MD_CHECK(pthread_cond_init(&a11y->change_cv, NULL) == 0);
}

static void query_a11y_fini(MdA11yCtx *a11y)
{
    pthread_cond_destroy(&a11y->change_cv);
    pthread_mutex_destroy(&a11y->change_mu);
    pthread_mutex_destroy(&a11y->snapshot_mu);
}

static MdA11yNode *query_node(const char *id, const char *role,
                              const char *label, int children)
{
    MdA11yNode *node = calloc(1, sizeof(*node));
    MD_CHECK(node != NULL);
    node->id = strdup(id);
    node->role = strdup(role);
    node->label = label ? strdup(label) : NULL;
    node->x = 10; node->y = 20; node->w = 300; node->h = 200;
    node->child_count = children;
    if (children) node->children = calloc((size_t)children, sizeof(*node->children));
    return node;
}

static int query_get_tree(MdA11yCtx *ctx, MdA11yNode **out)
{
    QueryFixture *fixture = ctx->backend_data;
    fixture->walks++;
    if (fixture->invalidate_during_walk) {
        md_a11y_snapshot_invalidate(fixture->invalidate_during_walk);
        fixture->invalidate_during_walk = NULL;
    }
    if (fixture->unavailable) return -1;
    if (fixture->modal_role) {
        MdA11yNode *root = query_node("root", "application", "Editor", 2);
        MdA11yNode *win = root->children[0] = query_node("win", "frame", "Main", 2);
        MdA11yNode *modal = win->children[0] = query_node("modal", fixture->modal_role, "Modal", 1);
        win->children[1] = query_node("nested", "window", "Ordinary nested window", 0);
        root->children[1] = query_node("top_modal", fixture->modal_role, "Standalone modal", 0);
        MdA11yNode *entry = modal->children[0] = query_node("entry", "entry", "Focused", 0);
        entry->states = calloc(2, sizeof(*entry->states));
        entry->states[0] = strdup("focused");
        entry->state_count = 1;
        *out = root;
        return 0;
    }
    if (fixture->unusual_nodes) {
        MdA11yNode *root = query_node("root", "application", "Editor", 2);
        root->children[0] = query_node("heading", "heading", "Welcome", 0);
        root->children[1] = query_node("unlabelled", "button", NULL, 0);
        *out = root;
        return 0;
    }
    if (fixture->many_buttons) {
        MdA11yNode *root = query_node("root", "application", "Many buttons", 60);
        for (int i = 0; i < 60; i++) {
            char id[32];
            snprintf(id, sizeof(id), "button_%d", i);
            root->children[i] = query_node(id, "button", "Button", 0);
        }
        *out = root;
        return 0;
    }
    if (fixture->bare_focus) {
        MdA11yNode *root = query_node("alone", "button", "Standalone", 0);
        root->states = calloc(2, sizeof(*root->states));
        root->states[0] = strdup("focused");
        root->state_count = 1;
        *out = root;
        return 0;
    }
    MdA11yNode *root = query_node("desktop", "desktop", "Desktop", 1);
    MdA11yNode *app = root->children[0] = query_node("app", "application", "Editor", 2);
    MdA11yNode *win = app->children[0] = query_node("win", "frame", "Editor window", 2);
    MdA11yNode *panel = win->children[0] = query_node("panel", "panel", "Controls", 3);
    MdA11yNode *save = panel->children[0] = query_node("save", "button", "Save As", 0);
    panel->children[1] = query_node("cancel", "button", "Cancel", 0);
    panel->children[2] = query_node("label", "label", "Saved files", 0);
    win->children[1] = query_node("nested", "window", "Nested window", 0);
    app->children[1] = query_node("other", "WINDOW", "Other window", 0);
    save->state_count = fixture->no_focus ? 1 : 2;
    save->states = calloc(3, sizeof(*save->states));
    save->states[0] = strdup("enabled");
    if (!fixture->no_focus) save->states[1] = strdup("focused");
    *out = root;
    return 0;
}

static int query_subscribe(MdA11yCtx *ctx, MdA11yChangeCb cb, void *userdata)
{
    QueryFixture *fixture = ctx->backend_data;
    if (fixture->subscribe_fails) return -1;
    fixture->subscribes++;
    fixture->change_cb = cb;
    fixture->change_userdata = userdata;
    return 0;
}

static void query_unsubscribe(MdA11yCtx *ctx)
{
    QueryFixture *fixture = ctx->backend_data;
    fixture->unsubscribes++;
    fixture->change_cb = NULL;
    fixture->change_userdata = NULL;
}

/* Simulate a backend change event exactly the way the real backends
 * dispatch one: through md_a11y_change_dispatch() on the ctx.
 * Returns whether a callback was registered before dispatch. */
static bool query_fire_change(MdA11yCtx *ctx)
{
    pthread_mutex_lock(&ctx->change_mu);
    bool subscribed = ctx->change_cb != NULL;
    pthread_mutex_unlock(&ctx->change_mu);
    md_a11y_change_dispatch(ctx);
    return subscribed;
}

static cJSON *query_call(MdMcpServer *server, const char *name,
                         const char *args, bool error)
{
    char *message = NULL;
    MD_CHECK(asprintf(&message,
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", name, args) >= 0);
    clear_responses();
    md_mcp_server_handle_message(server, message, strlen(message));
    free(message);
    cJSON *response = last_response();
    cJSON *result = cJSON_GetObjectItem(response, "result");
    MD_CHECK(cJSON_IsObject(result));
    MD_CHECK(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")) == error);
    cJSON *content = cJSON_GetObjectItem(result, "content");
    MD_CHECK(cJSON_GetArraySize(content) == 1);
    cJSON *item = cJSON_GetArrayItem(content, 0);
    MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(item, "type")), "text") == 0);
    MD_CHECK(cJSON_IsString(cJSON_GetObjectItem(item, "text")));
    cJSON *copy = cJSON_Duplicate(item, true);
    cJSON_Delete(response);
    return copy;
}

static const char *query_text(cJSON *content)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(content, "text"));
}

static cJSON *query_json(MdMcpServer *server, const char *name, const char *args)
{
    cJSON *content = query_call(server, name, args, false);
    cJSON *value = cJSON_Parse(query_text(content));
    MD_CHECK(value != NULL);
    cJSON_Delete(content);
    return value;
}

static void test_a11y_read_tools(void)
{
    QueryFixture fixture = {0};
    static const MdA11yBackend backend = { .get_tree = query_get_tree };
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    MdMcpServer *server = make_server();
    MdA11ySnapshot *snapshot = md_a11y_snapshot_create(&a11y);
    MD_CHECK(snapshot != NULL);
    MdMcpToolCtx ctx = { .snapshot = snapshot };
    MD_CHECK(md_mcp_register_tools(server, &ctx) == 0);
    do_init(server);

    cJSON *value = query_json(server, "metadesk_find", "{\"role\":\"BUTTON\",\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) == 2);
    MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(value, 0), "id")), "save") == 0);
    MD_CHECK(!cJSON_HasObjectItem(cJSON_GetArrayItem(value, 0), "children"));
    cJSON_Delete(value);
    value = query_json(server, "metadesk_find", "{\"label_contains\":\"sAvE\",\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) == 2);
    cJSON_Delete(value);
    value = query_json(server, "metadesk_find", "{\"state\":\"ENABLED\",\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) == 1);
    cJSON_Delete(value);
    value = query_json(server, "metadesk_find", "{\"state\":\"enable\",\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) == 0);
    cJSON_Delete(value);
    value = query_json(server, "metadesk_find", "{\"role\":\"button\",\"label_contains\":\"save\",\"state\":\"focused\",\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) == 1);
    cJSON_Delete(value);
    value = query_json(server, "metadesk_find", "{\"role\":\"button\",\"limit\":1,\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) == 1);
    cJSON_Delete(value);
    cJSON *content = query_call(server, "metadesk_find", "{\"label_contains\":\"save\",\"limit\":1}", false);
    MD_CHECK(strstr(query_text(content), "BTN[save]") != NULL);
    MD_CHECK(strstr(query_text(content), "[label]") == NULL);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_find", "{\"role\":\"label\"}", false);
    MD_CHECK(strstr(query_text(content), "LBL[label]") != NULL);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_find", "{\"role\":\"missing\"}", false);
    MD_CHECK(strcmp(query_text(content), "") == 0);
    cJSON_Delete(content);

    for (int depth = 0; depth <= 2; depth++) {
        char args[128];
        snprintf(args, sizeof(args), "{\"id\":\"win\",\"depth\":%d,\"format\":\"json\"}", depth);
        value = query_json(server, "metadesk_subtree", args);
        cJSON *children = cJSON_GetObjectItem(value, "children");
        MD_CHECK(cJSON_GetArraySize(children) == (depth ? 2 : 0));
        if (depth) {
            cJSON *grandchildren = cJSON_GetObjectItem(cJSON_GetArrayItem(children, 0), "children");
            MD_CHECK(cJSON_GetArraySize(grandchildren) == (depth == 2 ? 3 : 0));
            if (depth == 2)
                MD_CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetArrayItem(grandchildren, 0), "children")) == 0);
        }
        cJSON_Delete(value);
    }
    content = query_call(server, "metadesk_subtree", "{\"id\":\"win\"}", false);
    MD_CHECK(strstr(query_text(content), "WIN[win]") != NULL);
    MD_CHECK(strstr(query_text(content), "PNL[panel]") != NULL);
    MD_CHECK(strstr(query_text(content), "BTN[save]") != NULL);
    MD_CHECK(strstr(query_text(content), "WIN[other]") == NULL);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_subtree", "{\"id\":\"win\",\"depth\":0}", false);
    MD_CHECK(strstr(query_text(content), "PNL[panel]") == NULL);
    cJSON_Delete(content);

    value = query_json(server, "metadesk_node", "{\"id\":\"panel\"}");
    MD_CHECK(cJSON_GetObjectItem(value, "child_count")->valueint == 3);
    MD_CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(value, "state")) == 0);
    MD_CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(value, "bounds"), "w")->valueint == 300);
    MD_CHECK(!cJSON_HasObjectItem(value, "children"));
    cJSON_Delete(value);
    value = query_json(server, "metadesk_focused", "{}");
    MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(value, "node"), "id")), "save") == 0);
    MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(value, "window"), "id")), "win") == 0);
    cJSON_Delete(value);
    value = query_json(server, "metadesk_windows", "{}");
    MD_CHECK(cJSON_GetArraySize(value) == 2);
    MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(value, 1), "id")), "other") == 0);
    MD_CHECK(!cJSON_HasObjectItem(cJSON_GetArrayItem(value, 0), "state"));
    cJSON_Delete(value);

    content = query_call(server, "metadesk_node", "{\"id\":\"missing\"}", true);
    MD_CHECK(strstr(query_text(content), "not found") != NULL);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_subtree", "{\"id\":\"missing\"}", true);
    cJSON_Delete(content);
    fixture.many_buttons = true;
    md_a11y_snapshot_invalidate(snapshot);
    value = query_json(server, "metadesk_find", "{\"role\":\"button\",\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) == 50);
    cJSON_Delete(value);
    value = query_json(server, "metadesk_find", "{\"role\":\"button\",\"limit\":51,\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) == 51);
    cJSON_Delete(value);
    content = query_call(server, "metadesk_find", "{\"role\":\"button\"}", false);
    int lines = 0;
    for (const char *p = query_text(content); *p; p++) if (*p == '\n') lines++;
    MD_CHECK(lines == 50);
    cJSON_Delete(content);
    fixture.many_buttons = false;
    fixture.bare_focus = true;
    md_a11y_snapshot_invalidate(snapshot);
    value = query_json(server, "metadesk_focused", "{}");
    MD_CHECK(cJSON_IsNull(cJSON_GetObjectItem(value, "window")));
    cJSON_Delete(value);
    value = query_json(server, "metadesk_windows", "{}");
    MD_CHECK(cJSON_GetArraySize(value) == 0);
    cJSON_Delete(value);
    fixture.bare_focus = false;
    md_a11y_snapshot_invalidate(snapshot);
    value = query_json(server, "metadesk_find", "{\"limit\":500,\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) > 0);
    cJSON_Delete(value);
    value = query_json(server, "metadesk_subtree", "{\"id\":\"win\",\"depth\":64,\"format\":\"json\"}");
    MD_CHECK(cJSON_IsObject(value));
    cJSON_Delete(value);
    int walks = fixture.walks;
    const char *bad_args[] = {
        "{\"limit\":0}", "{\"limit\":1.5}", "{\"limit\":501}",
        "{\"limit\":2147483647}", "{\"limit\":2147483648}",
        "{\"format\":\"xml\"}", "{\"role\":2}", "{\"bogus\":true}", "[]"
    };
    for (size_t i = 0; i < sizeof(bad_args) / sizeof(bad_args[0]); i++) {
        content = query_call(server, "metadesk_find", bad_args[i], true);
        MD_CHECK(strstr(query_text(content), "Invalid") != NULL);
        const char *field = i < 5 ? "'limit'" : i == 5 ? "'format'" :
                            i == 6 ? "'role'" : i == 7 ? "'bogus'" : "'arguments'";
        MD_CHECK(strstr(query_text(content), field) != NULL);
        MD_CHECK(strstr(query_text(content), "serialize") == NULL);
        cJSON_Delete(content);
    }
    content = query_call(server, "metadesk_subtree", "{\"id\":\"win\",\"depth\":-1}", true);
    MD_CHECK(strstr(query_text(content), "'depth'") != NULL);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_subtree", "{\"id\":\"win\",\"depth\":65}", true);
    MD_CHECK(strstr(query_text(content), "Invalid") != NULL);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_node", "{}", true);
    MD_CHECK(strstr(query_text(content), "'id'") != NULL);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_windows", "{\"id\":\"win\"}", true);
    cJSON_Delete(content);
    MD_CHECK(fixture.walks == walks); /* reject bad args before walking */
    fixture.no_focus = true;
    md_a11y_snapshot_invalidate(snapshot);
    content = query_call(server, "metadesk_focused", "{}", true);
    cJSON_Delete(content);
    MD_CHECK(fixture.walks == walks + 1); /* refresh after invalidation */
    fixture.unavailable = true;
    md_a11y_snapshot_invalidate(snapshot);
    content = query_call(server, "metadesk_find", "{}", true);
    MD_CHECK(strstr(query_text(content), "unavailable") != NULL);
    cJSON_Delete(content);
    md_mcp_server_destroy(server);
    md_mcp_tools_cleanup(&ctx);
    md_a11y_snapshot_destroy(snapshot);
    PASS("targeted a11y reads: filters, depth, detail, focus, windows and errors");
}

static void test_a11y_modal_and_label_queries(void)
{
    static const MdA11yBackend backend = { .get_tree = query_get_tree };
    QueryFixture fixture = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    MdMcpServer *server = make_server();
    MdA11ySnapshot *snapshot = md_a11y_snapshot_create(&a11y);
    MD_CHECK(snapshot != NULL);
    MdMcpToolCtx ctx = { .snapshot = snapshot };
    MD_CHECK(md_mcp_register_tools(server, &ctx) == 0);
    do_init(server);
    const char *roles[] = { "dialog", "ALERT" };
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        fixture.modal_role = roles[i];
        md_a11y_snapshot_invalidate(snapshot);
        cJSON *value = query_json(server, "metadesk_windows", "{}");
        MD_CHECK(cJSON_GetArraySize(value) == 3);
        MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(value, 0), "id")), "win") == 0);
        MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(value, 1), "id")), "modal") == 0);
        MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(value, 2), "id")), "top_modal") == 0);
        cJSON_Delete(value);
        value = query_json(server, "metadesk_focused", "{}");
        MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(value, "window"), "id")), "modal") == 0);
        cJSON_Delete(value);
    }
    fixture.modal_role = NULL;
    fixture.unusual_nodes = true;
    md_a11y_snapshot_invalidate(snapshot);
    cJSON *content = query_call(server, "metadesk_find", "{\"role\":\"heading\"}", false);
    MD_CHECK(strcmp(query_text(content), "heading[heading] Welcome\n") == 0);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_subtree", "{\"id\":\"root\"}", false);
    MD_CHECK(strstr(query_text(content), "heading[heading] Welcome") != NULL);
    cJSON_Delete(content);
    cJSON *all = query_json(server, "metadesk_find", "{\"format\":\"json\"}");
    cJSON *empty = query_json(server, "metadesk_find", "{\"label_contains\":\"\",\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(all) == 3);
    MD_CHECK(cJSON_Compare(all, empty, true));
    cJSON_Delete(all);
    cJSON_Delete(empty);
    cJSON *value = query_json(server, "metadesk_find", "{\"label_contains\":\"missing\",\"format\":\"json\"}");
    MD_CHECK(cJSON_GetArraySize(value) == 0);
    cJSON_Delete(value);
    MdA11yNode *root = md_a11y_walk(&a11y);
    char *compact = md_a11y_to_compact(root);
    MD_CHECK(compact != NULL);
    MD_CHECK(strstr(compact, "heading") == NULL); /* full-tree filtering is unchanged */
    MD_CHECK(strstr(compact, "BTN[unlabelled]") != NULL);
    free(compact);
    md_a11y_node_free(root);
    md_mcp_server_destroy(server);
    md_mcp_tools_cleanup(&ctx);
    md_a11y_snapshot_destroy(snapshot);
    PASS("modal ancestors/windows, raw compact roles and empty label filters");
}

static void test_resource_tree_formats(void)
{
    static const MdA11yBackend backend = {
        .get_tree = query_get_tree, .subscribe_changes = query_subscribe,
        .unsubscribe_changes = query_unsubscribe,
    };
    QueryFixture fixture = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    query_a11y_init(&a11y);
    for (int i = 0; i < 3; i++) {
        MdMcpBridgeConfig config = {
            .a11y = &a11y,
            .tree_format = (MdMcpTreeFormat)i,
            .settle_ms = 1,
            .stdio_in_fd = -1,
            .stdio_out_fd = -1,
        };
        MdMcpBridge *bridge = md_mcp_bridge_create(&config);
        MD_CHECK(bridge != NULL);
        MdMcpServer *server = md_mcp_bridge_get_server(bridge);
        md_mcp_server_set_write_fn(server, capture_write, NULL);
        do_init(server);
        const char *mime = i == MD_MCP_TREE_FORMAT_JSON ? "application/json" : "text/plain";
        const char *requests[] = {
            "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"resources/list\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"resources/read\",\"params\":{\"uri\":\"metadesk://ui-tree\"}}"
        };
        uint64_t first_ts = 0;
        for (int j = 0; j < 4; j++) {
            fixture.unavailable = j == 3;
            if (j == 3) MD_CHECK(query_fire_change(&a11y));
            clear_responses();
            const char *request = requests[j == 0 ? 0 : 1];
            md_mcp_server_handle_message(server, request, strlen(request));
            cJSON *response = last_response();
            cJSON *result = cJSON_GetObjectItem(response, "result");
            cJSON *items = cJSON_GetObjectItem(result, j == 0 ? "resources" : "contents");
            cJSON *item = cJSON_GetArrayItem(items, 0);
            MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(item, "mimeType")), mime) == 0);
            if (j == 1 || j == 2) {
                const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "text"));
                uint64_t ts;
                if (i == MD_MCP_TREE_FORMAT_JSON) {
                    cJSON *tree = cJSON_Parse(text);
                    MD_CHECK(cJSON_IsObject(cJSON_GetObjectItem(tree, "root")));
                    ts = (uint64_t)cJSON_GetObjectItem(tree, "ts")->valuedouble;
                    cJSON_Delete(tree);
                } else {
                    MD_CHECK(strncmp(text, "v1 ts:", 6) == 0);
                    ts = strtoull(text + 6, NULL, 10);
                }
                MD_CHECK(ts > 0);
                if (j == 1) first_ts = ts;
                else MD_CHECK(ts == first_ts); /* cached walk, not read time */
            } else if (j == 3) {
                const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "text"));
                MD_CHECK(strcmp(text, i == MD_MCP_TREE_FORMAT_JSON ? "{\"root\":null}" : "") == 0);
            }
            cJSON_Delete(response);
        }
        fixture.unavailable = false;
        cJSON *content = query_call(server, "metadesk_find", "{\"role\":\"label\"}", false);
        MD_CHECK(strstr(query_text(content), "LBL[label]") != NULL);
        cJSON_Delete(content);
        /* First action has no diff baseline, forcing the full-tree fallback. */
        MD_CHECK(a11y.last_snapshot == NULL);
        cJSON *update = query_json(server, "metadesk_key_combo", "{\"keys\":[\"ctrl\",\"s\"]}");
        MD_CHECK(cJSON_IsObject(update));
        MD_CHECK(cJSON_GetObjectItem(update, "v")->valueint == 1);
        MD_CHECK(cJSON_IsObject(cJSON_GetObjectItem(update, "root")));
        cJSON_Delete(update);
        /* Subsequent changes still produce JSON deltas in compact sessions. */
        fixture.no_focus = true;
        update = query_json(server, "metadesk_key_combo", "{\"keys\":[\"ctrl\",\"s\"]}");
        MD_CHECK(cJSON_IsArray(update));
        MD_CHECK(cJSON_GetArraySize(update) > 0);
        cJSON_Delete(update);
        fixture.no_focus = false;
        md_mcp_bridge_destroy(bridge);
        md_a11y_node_free(a11y.last_snapshot);
        a11y.last_snapshot = NULL;
    }
    MD_CHECK(fixture.unsubscribes == 3);
    query_a11y_fini(&a11y);
    PASS("resource formats and JSON action fallback/deltas for default/compact/JSON bridges");
}

static void read_cached_resource(MdMcpServer *server)
{
    const char request[] = "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"resources/read\","
                           "\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    clear_responses();
    md_mcp_server_handle_message(server, request, sizeof(request) - 1);
    cJSON *response = last_response();
    cJSON *contents = cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "contents");
    MD_CHECK(cJSON_GetArraySize(contents) == 1);
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(contents, 0), "text"));
    MD_CHECK(text && strncmp(text, "v1 ts:", 6) == 0);
    cJSON_Delete(response);
}

static void test_shared_a11y_snapshot(void)
{
    static const MdA11yBackend backend = {
        .get_tree = query_get_tree, .subscribe_changes = query_subscribe,
        .unsubscribe_changes = query_unsubscribe,
    };
    QueryFixture fixture = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    query_a11y_init(&a11y);
    MdMcpBridgeConfig config = {
        .a11y = &a11y, .settle_ms = 1, .notify_sync = true,
        .stdio_in_fd = -1, .stdio_out_fd = -1,
    };
    MdMcpBridge *bridge = md_mcp_bridge_create(&config);
    MD_CHECK(bridge != NULL);
    MD_CHECK(fixture.change_cb != NULL);
    MD_CHECK(fixture.walks == 0); /* cache creation is lazy */
    MdMcpServer *server = md_mcp_bridge_get_server(bridge);
    md_mcp_server_set_write_fn(server, capture_write, NULL);
    do_init(server);
    cJSON *value = query_json(server, "metadesk_find", "{\"role\":\"button\",\"format\":\"json\"}");
    cJSON_Delete(value);
    value = query_json(server, "metadesk_node", "{\"id\":\"save\"}");
    cJSON_Delete(value);
    cJSON *content = query_call(server, "metadesk_subtree", "{\"id\":\"win\"}", false);
    cJSON_Delete(content);
    MD_CHECK(fixture.walks == 1);
    value = query_json(server, "metadesk_focused", "{}");
    cJSON_Delete(value);
    value = query_json(server, "metadesk_windows", "{}");
    cJSON_Delete(value);
    read_cached_resource(server);
    MD_CHECK(fixture.walks == 1); /* all five tools and resource share it */
    MD_CHECK(a11y.last_snapshot == NULL); /* reads do not seed diff state */

    const char subscribe[] = "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"resources/subscribe\","
                             "\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    md_mcp_server_handle_message(server, subscribe, sizeof(subscribe) - 1);
    clear_responses();
    fixture.no_focus = true;
    MD_CHECK(query_fire_change(&a11y));
    cJSON *notification = last_response();
    MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(notification, "method")),
                    "notifications/resources/updated") == 0);
    cJSON_Delete(notification);
    MD_CHECK(fixture.walks == 1); /* invalidation itself does not walk */
    read_cached_resource(server);
    MD_CHECK(fixture.walks == 2);
    content = query_call(server, "metadesk_focused", "{}", true);
    MD_CHECK(strstr(query_text(content), "not found") != NULL);
    cJSON_Delete(content);
    MD_CHECK(fixture.walks == 2);

    value = query_json(server, "metadesk_key_combo", "{\"keys\":[\"ctrl\",\"s\"]}");
    cJSON_Delete(value);
    int walks = fixture.walks; /* action pipeline has its own direct walks */
    value = query_json(server, "metadesk_node", "{\"id\":\"save\"}");
    cJSON_Delete(value);
    MD_CHECK(fixture.walks == walks + 1);
    read_cached_resource(server);
    MD_CHECK(fixture.walks == walks + 1);

    content = query_call(server, "metadesk_click", "{\"target_id\":\"missing\"}", true);
    cJSON_Delete(content);
    walks = fixture.walks;
    read_cached_resource(server); /* failed action attempts also invalidate */
    MD_CHECK(fixture.walks == walks + 1);

    md_mcp_bridge_destroy(bridge);
    md_a11y_node_free(a11y.last_snapshot);
    query_a11y_fini(&a11y);
    PASS("shared lazy snapshot: five reads, resource, change callback and actions");
}

static void test_snapshot_invalidation_and_time(void)
{
    static const MdA11yBackend backend = { .get_tree = query_get_tree };
    QueryFixture fixture = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    MdA11ySnapshot *snapshot = md_a11y_snapshot_create(&a11y);
    MD_CHECK(snapshot != NULL);
    fixture.invalidate_during_walk = snapshot;
    const MdA11yNode *root = md_a11y_snapshot_acquire(snapshot);
    MD_CHECK(root != NULL && fixture.walks == 1);
    MD_CHECK(md_a11y_snapshot_time_ms(snapshot) > 0);

    const uint64_t fixed_ts = 1234567890123ULL;
    char *json = md_a11y_to_json_at(root, fixed_ts);
    cJSON *tree = cJSON_Parse(json);
    MD_CHECK(tree != NULL);
    MD_CHECK(cJSON_GetObjectItem(tree, "ts")->valuedouble == (double)fixed_ts);
    cJSON_Delete(tree);
    free(json);
    char *compact = md_a11y_to_compact_at(root, fixed_ts);
    MD_CHECK(compact && strncmp(compact, "v1 ts:1234567890123\n", 20) == 0);
    free(compact);
    md_a11y_snapshot_release(snapshot);

    /* The invalidation inside get_tree must survive completion of that walk. */
    MD_CHECK(md_a11y_snapshot_acquire(snapshot) != NULL);
    MD_CHECK(fixture.walks == 2);
    uint64_t time_ms = md_a11y_snapshot_time_ms(snapshot);
    md_a11y_snapshot_release(snapshot);
    MD_CHECK(md_a11y_snapshot_acquire(snapshot) != NULL);
    MD_CHECK(fixture.walks == 2 && md_a11y_snapshot_time_ms(snapshot) == time_ms);
    /* Must return even while this thread holds the read lock. */
    md_a11y_snapshot_invalidate(snapshot);
    md_a11y_snapshot_release(snapshot);
    MD_CHECK(md_a11y_snapshot_acquire(snapshot) != NULL);
    MD_CHECK(fixture.walks == 3);
    md_a11y_snapshot_release(snapshot);

    fixture.unavailable = true;
    md_a11y_snapshot_invalidate(snapshot);
    MD_CHECK(md_a11y_snapshot_acquire(snapshot) == NULL);
    MD_CHECK(md_a11y_snapshot_time_ms(snapshot) == 0);
    md_a11y_snapshot_release(snapshot);
    fixture.unavailable = false;
    MD_CHECK(md_a11y_snapshot_acquire(snapshot) != NULL);
    MD_CHECK(fixture.walks == 5);
    md_a11y_snapshot_release(snapshot);
    md_a11y_snapshot_destroy(snapshot);
    PASS("non-blocking invalidation during walk/read and snapshot timestamps");
}

static void test_snapshot_subscribe_failure(void)
{
    const MdA11yBackend backends[] = {
        { .get_tree = query_get_tree, .subscribe_changes = query_subscribe,
          .unsubscribe_changes = query_unsubscribe },
        { .get_tree = query_get_tree },
    };
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        QueryFixture fixture = { .subscribe_fails = true };
        MdA11yCtx a11y = { .vtable = &backends[i], .backend_data = &fixture };
        query_a11y_init(&a11y);
        MdMcpBridgeConfig config = {
            .a11y = &a11y, .settle_ms = 1,
            .stdio_in_fd = -1, .stdio_out_fd = -1,
        };
        MdMcpBridge *bridge = md_mcp_bridge_create(&config);
        MD_CHECK(bridge != NULL && fixture.walks == 0);
        MdMcpServer *server = md_mcp_bridge_get_server(bridge);
        md_mcp_server_set_write_fn(server, capture_write, NULL);
        do_init(server);
        cJSON *value = query_json(server, "metadesk_find", "{\"format\":\"json\"}");
        cJSON_Delete(value);
        MD_CHECK(fixture.walks == 1);
        value = query_json(server, "metadesk_node", "{\"id\":\"save\"}");
        cJSON_Delete(value);
        MD_CHECK(fixture.walks == 2);
        cJSON *content = query_call(server, "metadesk_subtree", "{\"id\":\"win\"}", false);
        cJSON_Delete(content);
        MD_CHECK(fixture.walks == 3);
        read_cached_resource(server);
        MD_CHECK(fixture.walks == 4);
        read_cached_resource(server);
        MD_CHECK(fixture.walks == 5);
        md_mcp_bridge_destroy(bridge);
        /* Nothing was subscribed, so destroy must not try to detach. */
        MD_CHECK(fixture.unsubscribes == 0);
        MD_CHECK(a11y.change_cb == NULL);
        query_a11y_fini(&a11y);
    }
    PASS("failed or unsupported change subscription refreshes every read");
}

/* ── Unsubscribe / bridge teardown (md-6h0) ─────────────────────── */

static void test_bridge_destroy_unsubscribes(void)
{
    static const MdA11yBackend backend = {
        .get_tree = query_get_tree, .subscribe_changes = query_subscribe,
        .unsubscribe_changes = query_unsubscribe,
    };
    QueryFixture fixture = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    query_a11y_init(&a11y);

    /* Nothing subscribed yet: unsubscribe reports failure, backend untouched. */
    MD_CHECK(md_a11y_unsubscribe_changes(&a11y) == -1);
    MD_CHECK(fixture.unsubscribes == 0);

    MdMcpBridgeConfig config = {
        .a11y = &a11y, .settle_ms = 1,
        .stdio_in_fd = -1, .stdio_out_fd = -1,
    };
    MdMcpBridge *bridge = md_mcp_bridge_create(&config);
    MD_CHECK(bridge != NULL);
    md_mcp_server_set_write_fn(md_mcp_bridge_get_server(bridge), capture_write, NULL);
    MD_CHECK(a11y.change_cb != NULL && fixture.change_cb != NULL);
    MD_CHECK(query_fire_change(&a11y)); /* live: a change reaches the bridge */

    md_mcp_bridge_destroy(bridge);

    /* Destroy detached the callback before freeing the bridge... */
    MD_CHECK(fixture.unsubscribes == 1);
    MD_CHECK(a11y.change_cb == NULL && a11y.change_userdata == NULL);
    MD_CHECK(fixture.change_cb == NULL);
    /* ...so a late backend event finds nothing to invoke. */
    MD_CHECK(!query_fire_change(&a11y));
    MD_CHECK(a11y.change_in_flight == 0);
    /* And a second unsubscribe is a no-op failure, not a double teardown. */
    MD_CHECK(md_a11y_unsubscribe_changes(&a11y) == -1);
    MD_CHECK(fixture.unsubscribes == 1);

    md_a11y_node_free(a11y.last_snapshot);
    query_a11y_fini(&a11y);
    PASS("bridge destroy detaches the a11y change callback; unsubscribe without a subscription fails");
}

/* Counting latch (unnamed POSIX semaphores are unavailable on macOS). */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int count;
} TestLatch;

static void latch_init(TestLatch *l)
{
    MD_CHECK(pthread_mutex_init(&l->mu, NULL) == 0);
    MD_CHECK(pthread_cond_init(&l->cv, NULL) == 0);
    l->count = 0;
}

static void latch_fini(TestLatch *l)
{
    pthread_cond_destroy(&l->cv);
    pthread_mutex_destroy(&l->mu);
}

static void latch_post(TestLatch *l)
{
    pthread_mutex_lock(&l->mu);
    l->count++;
    pthread_cond_broadcast(&l->cv);
    pthread_mutex_unlock(&l->mu);
}

static void latch_wait(TestLatch *l)
{
    pthread_mutex_lock(&l->mu);
    while (l->count == 0)
        pthread_cond_wait(&l->cv, &l->mu);
    l->count--;
    pthread_mutex_unlock(&l->mu);
}

static int latch_count(TestLatch *l)
{
    pthread_mutex_lock(&l->mu);
    int count = l->count;
    pthread_mutex_unlock(&l->mu);
    return count;
}

typedef struct {
    MdA11yCtx *a11y;
    TestLatch entered;        /* callback -> test: I am running            */
    TestLatch release;        /* test -> callback: you may return          */
    TestLatch unsub_started;  /* unsubscriber -> test: calling unsubscribe */
    TestLatch unsub_returned; /* unsubscriber -> all: unsubscribe returned */
    int calls;
    int unsub_rc;
} DrainFixture;

static void drain_cb(const MdA11yDelta *deltas, int count, void *userdata)
{
    (void)deltas; (void)count;
    DrainFixture *d = userdata;
    d->calls++;
    latch_post(&d->entered);
    latch_wait(&d->release);
    /* Only widens the window for a non-draining unsubscribe to be caught;
     * the check below is the correctness mechanism, not the sleep. */
    usleep(10 * 1000);
    /* Drain guarantee: unsubscribe must still be blocked while we run. */
    MD_CHECK(latch_count(&d->unsub_returned) == 0);
}

static void *drain_dispatch_thread(void *arg)
{
    DrainFixture *d = arg;
    MD_CHECK(query_fire_change(d->a11y)); /* backend dispatch thread */
    return NULL;
}

static void *drain_unsubscribe_thread(void *arg)
{
    DrainFixture *d = arg;
    latch_post(&d->unsub_started);
    d->unsub_rc = md_a11y_unsubscribe_changes(d->a11y);
    latch_post(&d->unsub_returned);
    return NULL;
}

static void test_unsubscribe_drains_in_flight_callback(void)
{
    static const MdA11yBackend backend = {
        .get_tree = query_get_tree, .subscribe_changes = query_subscribe,
        .unsubscribe_changes = query_unsubscribe,
    };
    QueryFixture fixture = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    query_a11y_init(&a11y);

    DrainFixture d = { .a11y = &a11y };
    latch_init(&d.entered);
    latch_init(&d.release);
    latch_init(&d.unsub_started);
    latch_init(&d.unsub_returned);

    MD_CHECK(md_a11y_subscribe_changes(&a11y, drain_cb, &d) == 0);
    MD_CHECK(a11y.change_cb == drain_cb && a11y.change_userdata == &d);

    pthread_t dispatcher, unsubscriber;
    MD_CHECK(pthread_create(&dispatcher, NULL, drain_dispatch_thread, &d) == 0);
    latch_wait(&d.entered); /* the callback is now in flight and blocked */
    pthread_mutex_lock(&a11y.change_mu);
    MD_CHECK(a11y.change_in_flight == 1);
    pthread_mutex_unlock(&a11y.change_mu);

    MD_CHECK(pthread_create(&unsubscriber, NULL, drain_unsubscribe_thread, &d) == 0);
    latch_wait(&d.unsub_started);
    latch_post(&d.release); /* let the callback finish while unsubscribe waits */

    MD_CHECK(pthread_join(unsubscriber, NULL) == 0);
    MD_CHECK(pthread_join(dispatcher, NULL) == 0);

    MD_CHECK(d.unsub_rc == 0);
    MD_CHECK(d.calls == 1);
    MD_CHECK(latch_count(&d.unsub_returned) == 1);
    MD_CHECK(a11y.change_cb == NULL && a11y.change_in_flight == 0);
    MD_CHECK(fixture.unsubscribes == 1);
    MD_CHECK(!query_fire_change(&a11y));
    MD_CHECK(d.calls == 1);

    latch_fini(&d.unsub_returned);
    latch_fini(&d.unsub_started);
    latch_fini(&d.release);
    latch_fini(&d.entered);
    query_a11y_fini(&a11y);
    PASS("unsubscribe drains an in-flight change callback before returning");
}

/* Timed condition waits are watchdogs/assertions, never scheduling sleeps. */
static bool latch_wait_ms(TestLatch *l, int timeout_ms)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return false;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&l->mu);
    while (l->count == 0) {
        int rc = pthread_cond_timedwait(&l->cv, &l->mu, &deadline);
        if (rc == ETIMEDOUT) break;
        if (rc != 0) {
            pthread_mutex_unlock(&l->mu);
            return false;
        }
    }
    bool ready = l->count > 0;
    if (ready) l->count--;
    pthread_mutex_unlock(&l->mu);
    return ready;
}

typedef struct {
    MdMcpServer *server;
    TestLatch entered, release, notified;
    int request_writes, default_writes;
    int notify_rc, set_rc, quiesced_rc, dispatch_rc;
    bool released;
} SinkFixture;

static int request_sink(const char *json, size_t len, void *userdata)
{
    (void)json; (void)len;
    ((SinkFixture *)userdata)->request_writes++;
    return 0;
}

static int default_sink(const char *json, size_t len, void *userdata)
{
    (void)json; (void)len;
    ((SinkFixture *)userdata)->default_writes++;
    return 0;
}

static cJSON *blocked_sink_tool(const cJSON *args, bool *is_error,
                                char **error_msg, void *userdata)
{
    (void)args; (void)is_error; (void)error_msg;
    SinkFixture *f = userdata;
    md_mcp_server_notify_resource_updated(f->server, MD_MCP_URI_UI_TREE);
    latch_post(&f->entered);
    f->released = latch_wait_ms(&f->release, 5000);
    return cJSON_CreateArray();
}

static void *sink_dispatch_thread(void *userdata)
{
    SinkFixture *f = userdata;
    const char request[] = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"blocked\"}}";
    f->dispatch_rc = md_mcp_server_handle_message_with_sink(
        f->server, request, sizeof(request) - 1, request_sink, f);
    return NULL;
}

static void *sink_notify_thread(void *userdata)
{
    SinkFixture *f = userdata;
    f->notify_rc = md_mcp_server_notify_resource_updated(f->server, MD_MCP_URI_UI_TREE);
    f->set_rc = md_mcp_server_set_write_fn(f->server, default_sink, f);
    md_mcp_server_quiesce(f->server);
    f->quiesced_rc = md_mcp_server_notify_resource_updated(f->server, MD_MCP_URI_UI_TREE);
    latch_post(&f->notified);
    return NULL;
}

static void test_notification_during_dispatch(void)
{
    SinkFixture f = {0};
    latch_init(&f.entered); latch_init(&f.release); latch_init(&f.notified);
    MdMcpServerConfig cfg = { .write_fn = default_sink, .write_userdata = &f };
    f.server = md_mcp_server_create(&cfg);
    MD_CHECK(f.server != NULL);
    md_test_mcp_handshake(f.server);
    const char subscribe[] = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/subscribe\","
                             "\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    MD_CHECK(md_mcp_server_handle_message(f.server, subscribe, sizeof(subscribe) - 1) == 0);
    MdMcpTool tool = { .name = "blocked", .handler = blocked_sink_tool, .userdata = &f };
    MD_CHECK(md_mcp_server_register_tool(f.server, &tool) == 0);
    f.default_writes = 0;
    pthread_t dispatch, notify;
    MD_CHECK(pthread_create(&dispatch, NULL, sink_dispatch_thread, &f) == 0);
    MD_CHECK(latch_wait_ms(&f.entered, 2000));
    MD_CHECK(pthread_create(&notify, NULL, sink_notify_thread, &f) == 0);
    /* All three sink operations must finish while the tool still waits. */
    MD_CHECK(latch_wait_ms(&f.notified, 1000));
    latch_post(&f.release);
    MD_CHECK(pthread_join(notify, NULL) == 0);
    MD_CHECK(pthread_join(dispatch, NULL) == 0);
    MD_CHECK(f.released && f.dispatch_rc == 0);
    MD_CHECK(f.notify_rc == 0 && f.set_rc == 0 && f.quiesced_rc == -1);
    MD_CHECK(f.request_writes == 2 && f.default_writes == 1);
    md_mcp_server_destroy(f.server);
    latch_fini(&f.entered); latch_fini(&f.release); latch_fini(&f.notified);
    PASS("notification routing and quiesce do not wait for a blocked tool dispatch");
}

static void test_notification_quiesce(void)
{
    MdMcpServerConfig cfg = { .write_fn = capture_write };
    MdMcpServer *s = md_mcp_server_create(&cfg);
    MD_CHECK(s != NULL);
    md_test_mcp_handshake(s);
    const char subscribe[] = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/subscribe\","
                             "\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    MD_CHECK(md_mcp_server_handle_message(s, subscribe, sizeof(subscribe) - 1) == 0);
    clear_responses();
    MD_CHECK(md_mcp_server_notify_resource_updated(s, MD_MCP_URI_UI_TREE) == 0);
    MD_CHECK(g_response_count == 1);
    MD_CHECK(md_mcp_server_set_write_fn(s, NULL, NULL) == 0);
    MD_CHECK(md_mcp_server_notify_resource_updated(s, MD_MCP_URI_UI_TREE) == -1);
    MD_CHECK(g_response_count == 1);
    MD_CHECK(md_mcp_server_set_write_fn(s, capture_write, NULL) == 0);
    md_mcp_server_quiesce(s);
    md_mcp_server_quiesce(s); /* idempotent */
    MD_CHECK(md_mcp_server_notify_resource_updated(s, MD_MCP_URI_UI_TREE) == -1);
    MD_CHECK(md_mcp_server_notify_resource_updated(s, "unsubscribed") == -1);
    MD_CHECK(g_response_count == 1);
    md_mcp_server_destroy(s);
    clear_responses();
    PASS("quiesce rejects new notifications without invoking sink; NULL detaches");
}

typedef struct {
    QueryFixture query; /* first: reused fake backend callbacks */
    MdA11yCtx *a11y;
    MdMcpBridge *bridge;
    MdMcpServer *server;
    TestLatch entered, release, started, returned;
    int calls;
    bool released, stayed_alive, fired, quiesced;
} BridgeDrainFixture;

static int blocked_bridge_write(const char *json, size_t len, void *userdata)
{
    (void)json; (void)len;
    BridgeDrainFixture *f = userdata;
    f->calls++;
    if (f->calls == 1) {
        latch_post(&f->entered);
        f->released = latch_wait_ms(&f->release, 5000);
        f->stayed_alive = latch_count(&f->returned) == 0;
    }
    return 0;
}

static void bridge_drain_unsubscribe(MdA11yCtx *ctx)
{
    BridgeDrainFixture *f = ctx->backend_data;
    /* Destroy itself must quiesce; this test never calls quiesce manually. */
    f->quiesced = md_mcp_server_notify_resource_updated(f->server, MD_MCP_URI_UI_TREE) == -1;
    query_unsubscribe(ctx);
}

static void *bridge_change_thread(void *arg)
{
    BridgeDrainFixture *f = arg;
    f->fired = query_fire_change(f->a11y);
    return NULL;
}

static void *bridge_destroy_thread(void *arg)
{
    BridgeDrainFixture *f = arg;
    latch_post(&f->started);
    md_mcp_bridge_destroy(f->bridge);
    latch_post(&f->returned);
    return NULL;
}

typedef struct {
    MdA11yCtx *a11y;
    TestLatch started;
} HintFlood;

static void *hint_flood_thread(void *userdata)
{
    HintFlood *f = userdata;
    MD_CHECK(query_fire_change(f->a11y));
    latch_post(&f->started);
    /* Adversarial producer: stop only once destroy detaches the callback. */
    while (query_fire_change(f->a11y)) sched_yield();
    return NULL;
}

static void test_bridge_destroy_blocked_write(uint32_t coalesce_ms)
{
    static const MdA11yBackend backend = {
        .get_tree = query_get_tree, .subscribe_changes = query_subscribe,
        .unsubscribe_changes = bridge_drain_unsubscribe,
    };
    BridgeDrainFixture f = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &f };
    query_a11y_init(&a11y);
    f.a11y = &a11y;
    latch_init(&f.entered); latch_init(&f.release);
    latch_init(&f.started); latch_init(&f.returned);
    MdMcpBridgeConfig config = {
        .a11y = &a11y, .settle_ms = 1, .stdio_in_fd = -1, .stdio_out_fd = -1,
    };
    config.notify_coalesce_ms = coalesce_ms;
    config.notify_sync = coalesce_ms == 0;
    f.bridge = md_mcp_bridge_create(&config);
    MD_CHECK(f.bridge != NULL);
    f.server = md_mcp_bridge_get_server(f.bridge);
    MD_CHECK(md_mcp_server_set_write_fn(f.server, capture_write, NULL) == 0);
    md_test_mcp_handshake(f.server);
    const char subscribe[] = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/subscribe\","
                             "\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    MD_CHECK(md_mcp_server_handle_message(f.server, subscribe, sizeof(subscribe) - 1) == 0);
    clear_responses();
    MD_CHECK(md_mcp_server_set_write_fn(f.server, blocked_bridge_write, &f) == 0);

    pthread_t change_tid, destroy_tid;
    MD_CHECK(pthread_create(&change_tid, NULL, bridge_change_thread, &f) == 0);
    MD_CHECK(latch_wait_ms(&f.entered, 2000));
    /* Queue another burst while the notifier is inside its first sink.
     * Destroy must join that worker and discard its pending dirty state. */
    HintFlood flood = { .a11y = &a11y };
    pthread_t flood_tid;
    if (coalesce_ms) {
        for (int i = 0; i < 50; i++) MD_CHECK(query_fire_change(&a11y));
        latch_init(&flood.started);
        MD_CHECK(pthread_create(&flood_tid, NULL, hint_flood_thread, &flood) == 0);
        MD_CHECK(latch_wait_ms(&flood.started, 2000));
    }
    MD_CHECK(pthread_create(&destroy_tid, NULL, bridge_destroy_thread, &f) == 0);
    MD_CHECK(latch_wait_ms(&f.started, 2000));
    MD_CHECK(!latch_wait_ms(&f.returned, 100));
    latch_post(&f.release);
    MD_CHECK(latch_wait_ms(&f.returned, 2000));
    MD_CHECK(pthread_join(destroy_tid, NULL) == 0);
    MD_CHECK(pthread_join(change_tid, NULL) == 0);
    if (coalesce_ms) {
        MD_CHECK(pthread_join(flood_tid, NULL) == 0);
        latch_fini(&flood.started);
    }
    MD_CHECK(f.calls == 1 && f.query.unsubscribes == 1);
    MD_CHECK(f.released && f.stayed_alive && f.fired && f.quiesced);
    MD_CHECK(!query_fire_change(&a11y));
    MD_CHECK(a11y.change_in_flight == 0);
    latch_fini(&f.entered); latch_fini(&f.release);
    latch_fini(&f.started); latch_fini(&f.returned);
    md_a11y_node_free(a11y.last_snapshot);
    query_a11y_fini(&a11y);
    PASS("bridge destroy waits for blocked transport then completes within bound");
}

/* A sink latch counts only notifications, not handshake/read responses. */
static int hint_notify_write(const char *json, size_t len, void *userdata)
{
    (void)len;
    cJSON *message = cJSON_Parse(json);
    MD_CHECK(message != NULL);
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(message, "method"));
    MD_CHECK(method && strcmp(method, "notifications/resources/updated") == 0);
    latch_post(userdata);
    cJSON_Delete(message);
    return 0;
}

static void test_hint_coalescing(uint32_t quiet_ms, bool sync)
{
    static const MdA11yBackend backend = {
        .get_tree = query_get_tree, .subscribe_changes = query_subscribe,
        .unsubscribe_changes = query_unsubscribe,
    };
    QueryFixture fixture = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    query_a11y_init(&a11y);
    MdMcpBridgeConfig config = {
        .a11y = &a11y, .settle_ms = 1, .notify_coalesce_ms = quiet_ms,
        .notify_sync = sync,
        .stdio_in_fd = -1, .stdio_out_fd = -1,
    };
    MdMcpBridge *bridge = md_mcp_bridge_create(&config);
    MD_CHECK(bridge != NULL);
    MdMcpServer *server = md_mcp_bridge_get_server(bridge);
    md_mcp_server_set_write_fn(server, capture_write, NULL);
    md_test_mcp_handshake(server);
    const char subscribe[] = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/subscribe\","
                             "\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    MD_CHECK(md_mcp_server_handle_message(server, subscribe, sizeof(subscribe) - 1) == 0);
    const char read[] = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/read\","
                        "\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    MD_CHECK(md_mcp_server_handle_message(server, read, sizeof(read) - 1) == 0);
    int walks = fixture.walks;
    TestLatch notifications;
    latch_init(&notifications);
    md_mcp_server_set_write_fn(server, hint_notify_write, &notifications);

    for (int i = 0; i < 100; i++) MD_CHECK(query_fire_change(&a11y));
    MD_CHECK(fixture.walks == walks); /* hints never call get_tree */
    int expected = sync ? 100 : 1;
    for (int i = 0; i < expected; i++) MD_CHECK(latch_wait_ms(&notifications, 2000));
    MD_CHECK(!latch_wait_ms(&notifications, 60)); /* exactly one per burst */

    /* Read immediately after a hint, before the coalescer's quiet window:
     * invalidation must not be delayed until notification delivery. */
    MD_CHECK(query_fire_change(&a11y));
    MD_CHECK(md_mcp_server_handle_message_with_sink(server, read, sizeof(read) - 1,
                                            capture_write, NULL) == 0);
    MD_CHECK(fixture.walks == walks + 1);
    MD_CHECK(md_mcp_server_handle_message_with_sink(server, read, sizeof(read) - 1,
                                            capture_write, NULL) == 0);
    MD_CHECK(fixture.walks == walks + 1);
    MD_CHECK(latch_wait_ms(&notifications, 2000));
    MD_CHECK(!latch_wait_ms(&notifications, 60));

    /* A second 50-hint burst: disabled mode preserves 50 synchronous writes. */
    for (int i = 0; i < 50; i++) MD_CHECK(query_fire_change(&a11y));
    expected = sync ? 50 : 1;
    if (sync) MD_CHECK(latch_count(&notifications) == 50);
    for (int i = 0; i < expected; i++) MD_CHECK(latch_wait_ms(&notifications, 2000));
    MD_CHECK(!latch_wait_ms(&notifications, 60));

    /* Pending dirty state is discarded on teardown; no late sink calls. */
    md_mcp_server_quiesce(server);
    for (int i = 0; i < 50; i++) MD_CHECK(query_fire_change(&a11y));
    md_mcp_bridge_destroy(bridge);
    MD_CHECK(latch_count(&notifications) == 0);
    MD_CHECK(!query_fire_change(&a11y));
    MD_CHECK(fixture.unsubscribes == 1);
    latch_fini(&notifications);

    /* Fail transport creation after notifier startup and subscription. */
    config.stdio_in_fd = 0;
    config.stdio_out_fd = -1;
    MD_CHECK(md_mcp_bridge_create(&config) == NULL);
    MD_CHECK(fixture.unsubscribes == 2);
    MD_CHECK(!query_fire_change(&a11y));
    query_a11y_fini(&a11y);
    clear_responses();
    PASS("hints do zero walks, invalidate immediately, coalesce bursts and stop at quiesce");
}

static void test_hint_coalescing_cap(void)
{
    MdMcpServerConfig config = { .write_fn = capture_write };
    MdMcpServer *server = md_mcp_server_create(&config);
    MD_CHECK(server != NULL);
    md_test_mcp_handshake(server);
    const char subscribe[] = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/subscribe\","
                             "\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    MD_CHECK(md_mcp_server_handle_message(server, subscribe, sizeof(subscribe) - 1) == 0);
    TestLatch notifications;
    latch_init(&notifications);
    md_mcp_server_set_write_fn(server, hint_notify_write, &notifications);
    MdMcpResourceCtx ctx = { .server = server };
    ctx.notifier = md_mcp_notify_coalescer_create(server, 200);
    MD_CHECK(ctx.notifier != NULL);
    /* Keep the quiet deadline moving. A notification must arrive while
     * hints are still arriving, rather than waiting for the stream to end. */
    bool notified = false;
    for (int i = 0; i < 40; i++) {
        md_mcp_a11y_change_cb(NULL, 0, &ctx);
        if (latch_wait_ms(&notifications, 10)) { notified = true; break; }
    }
    MD_CHECK(notified);
    md_mcp_server_quiesce(server);
    md_mcp_notify_coalescer_destroy(ctx.notifier);
    md_mcp_server_destroy(server);
    latch_fini(&notifications);
    clear_responses();
    PASS("continuous hints notify at the maximum latency cap");
}

typedef struct {
    int fail_count;
    atomic_int calls;
    TestLatch attempts, delivered;
} RetrySink;

static int retry_notify_write(const char *json, size_t len, void *userdata)
{
    RetrySink *sink = userdata;
    int attempt = atomic_fetch_add(&sink->calls, 1) + 1;
    latch_post(&sink->attempts);
    if (attempt <= sink->fail_count) return -1;
    return hint_notify_write(json, len, &sink->delivered);
}

static void test_hint_notification_retry(int fail_count)
{
    MdMcpServerConfig config = { .write_fn = capture_write };
    MdMcpServer *server = md_mcp_server_create(&config);
    MD_CHECK(server != NULL);
    md_test_mcp_handshake(server);
    const char subscribe[] = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/subscribe\","
                             "\"params\":{\"uri\":\"metadesk://ui-tree\"}}";
    MD_CHECK(md_mcp_server_handle_message(server, subscribe, sizeof(subscribe) - 1) == 0);
    RetrySink sink = { .fail_count = fail_count };
    atomic_init(&sink.calls, 0);
    latch_init(&sink.attempts);
    latch_init(&sink.delivered);
    md_mcp_server_set_write_fn(server, retry_notify_write, &sink);
    MdMcpResourceCtx ctx = { .server = server };
    ctx.notifier = md_mcp_notify_coalescer_create(server, 20);
    MD_CHECK(ctx.notifier != NULL);
    md_mcp_a11y_change_cb(NULL, 0, &ctx);
    MD_CHECK(latch_wait_ms(&sink.attempts, 2000));
    /* Retries must wait a cap window, not spin on a failing sink. */
    MD_CHECK(!latch_wait_ms(&sink.attempts, 100));
    MD_CHECK(latch_wait_ms(&sink.attempts, 2000));
    MD_CHECK(latch_wait_ms(&sink.attempts, 2000));
    if (fail_count == 2) MD_CHECK(latch_wait_ms(&sink.delivered, 2000));
    MD_CHECK(!latch_wait_ms(&sink.attempts, 300));
    MD_CHECK(latch_count(&sink.delivered) == 0);
    MD_CHECK(atomic_load(&sink.calls) == 3);
    if (fail_count == 3) {
        /* Giving up one burst must not prevent delivery of the next. */
        md_mcp_a11y_change_cb(NULL, 0, &ctx);
        MD_CHECK(latch_wait_ms(&sink.delivered, 2000));
        MD_CHECK(atomic_load(&sink.calls) == 4);
    }
    md_mcp_server_quiesce(server);
    md_mcp_notify_coalescer_destroy(ctx.notifier);
    md_mcp_server_destroy(server);
    latch_fini(&sink.attempts);
    latch_fini(&sink.delivered);
    clear_responses();
    PASS("transient notification failures retry at 200 ms, bounded to three attempts");
}

static void counting_cb_a(const MdA11yDelta *deltas, int count, void *userdata)
{
    (void)deltas; (void)count;
    (*(int *)userdata)++;
}

static void counting_cb_b(const MdA11yDelta *deltas, int count, void *userdata)
{
    (void)deltas; (void)count;
    (*(int *)userdata) += 100;
}

static void test_subscribe_refuses_second_owner(void)
{
    static const MdA11yBackend backend = {
        .get_tree = query_get_tree, .subscribe_changes = query_subscribe,
        .unsubscribe_changes = query_unsubscribe,
    };
    QueryFixture fixture = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    query_a11y_init(&a11y);

    int first = 0, second = 0;
    MD_CHECK(md_a11y_subscribe_changes(&a11y, counting_cb_a, &first) == 0);
    MD_CHECK(fixture.subscribes == 1 && fixture.change_cb == counting_cb_a);

    /* A different callback, or the same callback with different userdata,
     * is refused and leaves the first subscription intact. */
    MD_CHECK(md_a11y_subscribe_changes(&a11y, counting_cb_b, &second) == -1);
    MD_CHECK(md_a11y_subscribe_changes(&a11y, counting_cb_a, &second) == -1);
    MD_CHECK(a11y.change_cb == counting_cb_a && a11y.change_userdata == &first);
    MD_CHECK(fixture.subscribes == 1);
    MD_CHECK(query_fire_change(&a11y));
    MD_CHECK(first == 1 && second == 0);

    /* Re-subscribing the identical pair is a no-op success. */
    MD_CHECK(md_a11y_subscribe_changes(&a11y, counting_cb_a, &first) == 0);
    MD_CHECK(fixture.subscribes == 1);

    MD_CHECK(md_a11y_unsubscribe_changes(&a11y) == 0);
    MD_CHECK(fixture.unsubscribes == 1);
    MD_CHECK(!query_fire_change(&a11y));
    MD_CHECK(first == 1 && second == 0);

    /* Two bridges on one ctx: the loser must not detach the winner. */
    MdMcpBridgeConfig config = {
        .a11y = &a11y, .settle_ms = 1,
        .stdio_in_fd = -1, .stdio_out_fd = -1,
    };
    MdMcpBridge *winner = md_mcp_bridge_create(&config);
    MD_CHECK(winner != NULL && fixture.subscribes == 2);
    MdMcpBridge *loser = md_mcp_bridge_create(&config);
    MD_CHECK(loser != NULL && fixture.subscribes == 2); /* refused, falls back */
    MdA11yChangeCb winner_cb = a11y.change_cb;
    void *winner_ud = a11y.change_userdata;
    MD_CHECK(winner_cb != NULL);
    md_mcp_bridge_destroy(loser);
    MD_CHECK(fixture.unsubscribes == 1); /* loser did not unsubscribe */
    MD_CHECK(a11y.change_cb == winner_cb && a11y.change_userdata == winner_ud);
    md_mcp_bridge_destroy(winner);
    MD_CHECK(fixture.unsubscribes == 2);
    MD_CHECK(a11y.change_cb == NULL);

    md_a11y_node_free(a11y.last_snapshot);
    query_a11y_fini(&a11y);
    PASS("second subscriber is refused; identical re-subscribe is a no-op; loser bridge does not detach winner");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--a11y-queries") == 0) {
        test_a11y_read_tools();
        test_a11y_modal_and_label_queries();
        test_shared_a11y_snapshot();
        test_snapshot_invalidation_and_time();
        test_snapshot_subscribe_failure();
        test_resource_tree_formats();
        test_bridge_destroy_unsubscribes();
        test_unsubscribe_drains_in_flight_callback();
        test_bridge_destroy_blocked_write(0);
        test_bridge_destroy_blocked_write(20);
        test_hint_coalescing(20, false);
        test_hint_coalescing(0, true);
        test_hint_coalescing(0, false);
        test_hint_notification_retry(2);
        test_hint_notification_retry(3);
        test_hint_coalescing_cap();
        test_subscribe_refuses_second_owner();
        clear_responses();
        return md_test_report();
    }
    printf("MCP server core tests:\n");

    test_initialize();
    test_reject_before_init();
    test_initialized_before_initialize_ignored();
    test_ping();
    test_tools_list();
    test_tools_call();
    test_tools_call_unknown();
    test_resources_list();
    test_resources_read();
    test_resources_subscribe();
    test_notification_quiesce();
    test_notification_during_dispatch();
    test_unknown_method();
    test_parse_error();
    test_init_capabilities_reflect_registrations();
    test_14_tools_register();
    test_tool_call_click_no_agent();
    test_tool_call_missing_target();
    test_tool_call_key_combo();
    test_tool_registration_failure_rolls_back(24);
    test_tool_registration_failure_rolls_back(19);
    test_resources_with_session();
    test_bridge_null_a11y_fails_create();
    test_bridge_create_destroy_with_stub_a11y();

    printf("\nAll MCP server tests passed.\n");
    clear_responses();
    return md_test_report();
}
