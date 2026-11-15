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
        .a11y = NULL,  /* no real a11y in test */
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
    MD_CHECK(cJSON_IsFalse(cJSON_GetObjectItem(find_schema, "additionalProperties")));
    cJSON *subtree_schema = cJSON_GetObjectItem(cJSON_GetArrayItem(tools, 10), "inputSchema");
    MD_CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(subtree_schema, "required")) == 1);
    props = cJSON_GetObjectItem(subtree_schema, "properties");
    MD_CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(props, "depth"), "default")->valueint == 2);
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
    bool unavailable, no_focus, many_buttons, bare_focus;
} QueryFixture;

static MdA11yNode *query_node(const char *id, const char *role,
                              const char *label, int children)
{
    MdA11yNode *node = calloc(1, sizeof(*node));
    MD_CHECK(node != NULL);
    node->id = strdup(id);
    node->role = strdup(role);
    node->label = strdup(label);
    node->x = 10; node->y = 20; node->w = 300; node->h = 200;
    node->child_count = children;
    if (children) node->children = calloc((size_t)children, sizeof(*node->children));
    return node;
}

static int query_get_tree(MdA11yCtx *ctx, MdA11yNode **out)
{
    QueryFixture *fixture = ctx->backend_data;
    fixture->walks++;
    if (fixture->unavailable) return -1;
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
    MdMcpToolCtx ctx = { .a11y = &a11y };
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
    value = query_json(server, "metadesk_focused", "{}");
    MD_CHECK(cJSON_IsNull(cJSON_GetObjectItem(value, "window")));
    cJSON_Delete(value);
    value = query_json(server, "metadesk_windows", "{}");
    MD_CHECK(cJSON_GetArraySize(value) == 0);
    cJSON_Delete(value);
    fixture.bare_focus = false;
    int walks = fixture.walks;
    const char *bad_args[] = {
        "{\"limit\":0}", "{\"limit\":1.5}", "{\"limit\":2147483648}",
        "{\"format\":\"xml\"}", "{\"role\":2}", "{\"bogus\":true}", "[]"
    };
    for (size_t i = 0; i < sizeof(bad_args) / sizeof(bad_args[0]); i++) {
        content = query_call(server, "metadesk_find", bad_args[i], true);
        cJSON_Delete(content);
    }
    content = query_call(server, "metadesk_subtree", "{\"id\":\"win\",\"depth\":-1}", true);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_node", "{}", true);
    cJSON_Delete(content);
    content = query_call(server, "metadesk_windows", "{\"id\":\"win\"}", true);
    cJSON_Delete(content);
    MD_CHECK(fixture.walks == walks); /* reject bad args before walking */
    fixture.no_focus = true;
    content = query_call(server, "metadesk_focused", "{}", true);
    cJSON_Delete(content);
    MD_CHECK(fixture.walks == walks + 1); /* fresh snapshot on every valid call */
    fixture.unavailable = true;
    content = query_call(server, "metadesk_find", "{}", true);
    MD_CHECK(strstr(query_text(content), "unavailable") != NULL);
    cJSON_Delete(content);
    md_mcp_server_destroy(server);
    md_mcp_tools_cleanup(&ctx);
    PASS("targeted a11y reads: filters, depth, detail, focus, windows and errors");
}

static void test_resource_tree_formats(void)
{
    static const MdA11yBackend backend = { .get_tree = query_get_tree };
    QueryFixture fixture = {0};
    MdA11yCtx a11y = { .vtable = &backend, .backend_data = &fixture };
    for (int i = 0; i < 3; i++) {
        MdMcpBridgeConfig config = {
            .a11y = &a11y,
            .tree_format = (MdMcpTreeFormat)i,
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
        for (int j = 0; j < 3; j++) {
            fixture.unavailable = j == 2;
            clear_responses();
            const char *request = requests[j == 0 ? 0 : 1];
            md_mcp_server_handle_message(server, request, strlen(request));
            cJSON *response = last_response();
            cJSON *result = cJSON_GetObjectItem(response, "result");
            cJSON *items = cJSON_GetObjectItem(result, j == 0 ? "resources" : "contents");
            cJSON *item = cJSON_GetArrayItem(items, 0);
            MD_CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(item, "mimeType")), mime) == 0);
            if (j == 1) {
                const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "text"));
                if (i == MD_MCP_TREE_FORMAT_JSON) {
                    cJSON *tree = cJSON_Parse(text);
                    MD_CHECK(cJSON_IsObject(cJSON_GetObjectItem(tree, "root")));
                    cJSON_Delete(tree);
                } else MD_CHECK(strncmp(text, "v1 ts:", 6) == 0);
            }
            cJSON_Delete(response);
        }
        fixture.unavailable = false;
        cJSON *content = query_call(server, "metadesk_find", "{\"role\":\"label\"}", false);
        MD_CHECK(strstr(query_text(content), "LBL[label]") != NULL);
        cJSON_Delete(content);
        md_mcp_bridge_destroy(bridge);
    }
    PASS("resource MIME and content match default/compact/JSON bridge formats");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--a11y-queries") == 0) {
        test_a11y_read_tools();
        test_resource_tree_formats();
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
