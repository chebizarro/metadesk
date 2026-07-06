/*
 * metadesk — tests/test_agent.c
 * Agent action handler tests.
 *
 * Tests the action parsing → target resolution → injection → delta pipeline.
 * AT-SPI2 and uinput may not be available in CI, so tests degrade gracefully.
 */
#include "agent.h"
#include "action.h"
#include "a11y.h"
#include "input.h"
#include "session.h"
#include "mcp_server.h"
#include "mcp_tools.h"
#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Mock input backend for deterministic injection failures ─── */

typedef struct {
    int mouse_move_calls;
    int mouse_move_ret;
    int mouse_button_calls;
    int mouse_button_ret;
    int type_text_calls;
    int type_text_ret;
} AgentMockInputData;

static int agent_mock_mouse_move(MdInputCtx *ctx, int x, int y) {
    AgentMockInputData *data = (AgentMockInputData *)ctx->backend_data;
    (void)x;
    (void)y;
    data->mouse_move_calls++;
    return data->mouse_move_ret;
}

static int agent_mock_mouse_button(MdInputCtx *ctx, int button, int pressed) {
    AgentMockInputData *data = (AgentMockInputData *)ctx->backend_data;
    (void)button;
    (void)pressed;
    data->mouse_button_calls++;
    return data->mouse_button_ret;
}

static int agent_mock_type_text(MdInputCtx *ctx, const char *utf8) {
    AgentMockInputData *data = (AgentMockInputData *)ctx->backend_data;
    (void)utf8;
    data->type_text_calls++;
    return data->type_text_ret;
}

static const MdInputBackend agent_mock_backend = {
    .mouse_move = agent_mock_mouse_move,
    .mouse_button = agent_mock_mouse_button,
    .type_text = agent_mock_type_text,
};

static MdInput make_agent_mock_input(AgentMockInputData *data) {
    MdInput inp;
    memset(&inp, 0, sizeof(inp));
    inp.vtable = &agent_mock_backend;
    inp.backend_data = data;
    inp.ready = true;
    return inp;
}

/* ── Capture transport for MCP tool-path assertions ──────────── */

static char *g_agent_mcp_response = NULL;

static void clear_agent_mcp_response(void) {
    free(g_agent_mcp_response);
    g_agent_mcp_response = NULL;
}

static int capture_agent_mcp_write(const char *json, size_t len, void *userdata) {
    (void)userdata;
    clear_agent_mcp_response();
    g_agent_mcp_response = strndup(json, len);
    return g_agent_mcp_response ? 0 : -1;
}

static MdMcpServer *make_agent_mcp_server(MdMcpToolCtx *tool_ctx) {
    clear_agent_mcp_response();
    MdMcpServerConfig cfg = {
        .server_name = "agent-test",
        .server_version = "0.0.1",
        .write_fn = capture_agent_mcp_write,
    };
    MdMcpServer *server = md_mcp_server_create(&cfg);
    assert(server != NULL);
    assert(md_mcp_register_tools(server, tool_ctx) == 0);

    const char *init = "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
                       "\"id\":1,\"params\":{\"protocolVersion\":\"2025-03-26\","
                       "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}";
    md_mcp_server_handle_message(server, init, strlen(init));
    const char *initialized = "{\"jsonrpc\":\"2.0\","
                              "\"method\":\"notifications/initialized\"}";
    md_mcp_server_handle_message(server, initialized, strlen(initialized));
    clear_agent_mcp_response();
    return server;
}

/* ── Test: create and destroy ────────────────────────────────── */

static int test_create_destroy(void) {
    printf("  test_create_destroy... ");

    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_COMPACT,
        .settle_ms   = 50,
    };

    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);
    assert(md_agent_get_action_count(agent) == 0);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: NULL config should fail ───────────────────────────── */

static int test_null_config(void) {
    printf("  test_null_config... ");
    assert(md_agent_create(NULL) == NULL);
    printf("OK\n");
    return 0;
}

/* ── Test: handle action with mock tree ──────────────────────── */

static int test_handle_action_no_deps(void) {
    printf("  test_handle_action_no_deps... ");

    /* Create agent with no AT-SPI2 or input — should still parse and not crash */
    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 10,  /* short settle for test speed */
    };

    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    /* We can't send packets without a real stream, but we can verify
     * the agent handles NULL stream gracefully */
    const char *json = "{\"v\":1,\"action\":\"click\",\"target_id\":\"n42\","
                       "\"payload\":{}}";
    int ret = md_agent_handle_action(agent, NULL, NULL,
                                     (const uint8_t *)json, (uint32_t)strlen(json));
    assert(ret == -1); /* should fail because stream is NULL */

    assert(md_agent_get_action_count(agent) == 0);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: action count tracking ─────────────────────────────── */

static int test_with_live_deps(void) {
    printf("  test_with_live_deps... ");

    /* Try to create real a11y and input contexts */
    MdA11yCtx *a11y = md_a11y_create();
    MdInput *input = md_input_create(NULL);

    if (!a11y) {
        printf("SKIP (no accessibility bus)\n");
        if (input) md_input_destroy(input);
        return 0;
    }

    MdAgentConfig cfg = {
        .a11y        = a11y,
        .input       = input,
        .tree_format = MD_TREE_FORMAT_COMPACT,
        .settle_ms   = 10,
    };

    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    /* Verify send_tree works with live AT-SPI2 but NULL stream
     * should return -1 (no stream) */
    uint32_t seq = 0;
    int ret = md_agent_send_tree(agent, NULL, &seq);
    assert(ret == -1);

    md_agent_destroy(agent);
    md_a11y_destroy(a11y);
    if (input) md_input_destroy(input);

    printf("OK\n");
    return 0;
}

/* ── Test: tree format configuration ──────────────────────────── */

static int test_tree_format_config(void) {
    printf("  test_tree_format_config... ");

    /* JSON format */
    MdAgentConfig cfg1 = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 50,
    };
    MdAgent *a1 = md_agent_create(&cfg1);
    assert(a1 != NULL);
    md_agent_destroy(a1);

    /* Compact format */
    MdAgentConfig cfg2 = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_COMPACT,
        .settle_ms   = 50,
    };
    MdAgent *a2 = md_agent_create(&cfg2);
    assert(a2 != NULL);
    md_agent_destroy(a2);

    printf("OK\n");
    return 0;
}

/* ── Test: send_tree and send_delta with NULL stream ─────────── */

static int test_send_tree_delta_null_stream(void) {
    printf("  test_send_tree_delta_null_stream... ");

    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 10,
    };
    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    uint32_t seq = 0;

    /* send_tree with NULL stream should fail gracefully */
    int ret = md_agent_send_tree(agent, NULL, &seq);
    assert(ret == -1);

    /* send_delta with NULL stream should fail gracefully */
    ret = md_agent_send_delta(agent, NULL, &seq);
    assert(ret == -1);

    /* seq should not have been incremented */
    assert(seq == 0);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: handle_action with various JSON payloads ──────────── */

static int test_handle_various_actions(void) {
    printf("  test_handle_various_actions... ");

    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 10,
    };
    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    /* All these should fail (no stream) but not crash */
    const char *actions[] = {
        "{\"v\":1,\"action\":\"click\",\"target_id\":\"btn1\",\"payload\":{}}",
        "{\"v\":1,\"action\":\"dbl_click\",\"target_id\":\"btn2\",\"payload\":{}}",
        "{\"v\":1,\"action\":\"type\",\"target_id\":\"inp1\",\"payload\":{\"text\":\"hi\"}}",
        "{\"v\":1,\"action\":\"key_combo\",\"payload\":{\"keys\":[\"ctrl\",\"a\"]}}",
        "{\"v\":1,\"action\":\"scroll\",\"target_id\":\"lst\",\"payload\":{\"dx\":0,\"dy\":-3}}",
        "{\"v\":1,\"action\":\"screenshot\",\"target_id\":\"win\",\"payload\":{\"region\":[0,0,640,480]}}",
    };

    for (int i = 0; i < 6; i++) {
        int ret = md_agent_handle_action(agent, NULL, NULL,
                                         (const uint8_t *)actions[i],
                                         (uint32_t)strlen(actions[i]));
        assert(ret == -1); /* no stream → fail */
    }

    /* Action count should still be 0 (none completed successfully) */
    assert(md_agent_get_action_count(agent) == 0);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: input injection failure propagates ────────────────── */

static int test_injection_failure_propagates(void) {
    printf("  test_injection_failure_propagates... ");

    AgentMockInputData data = { .type_text_ret = -1 };
    MdInput input = make_agent_mock_input(&data);
    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = &input,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 1,
    };
    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    const char *json = "{\"v\":1,\"action\":\"type\","
                       "\"payload\":{\"text\":\"hello\"}}";
    uint32_t seq = 7;
    int ret = md_agent_handle_action(agent, (MdStream *)0x1, &seq,
                                     (const uint8_t *)json,
                                     (uint32_t)strlen(json));
    assert(ret == -1);
    assert(seq == 7);
    assert(data.type_text_calls == 1);
    assert(md_agent_get_action_count(agent) == 0);
    md_agent_destroy(agent);

    data = (AgentMockInputData){ .type_text_ret = -1 };
    input = make_agent_mock_input(&data);
    cfg.input = &input;
    agent = md_agent_create(&cfg);
    assert(agent != NULL);

    char *result = md_agent_handle_action_mcp(agent,
                                              (const uint8_t *)json,
                                              (uint32_t)strlen(json));
    assert(result == NULL);
    assert(data.type_text_calls == 1);
    assert(md_agent_get_action_count(agent) == 0);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: MCP tool failure when a11y is unavailable ─────────── */

static int test_mcp_tool_requires_a11y_returns_error(void) {
    printf("  test_mcp_tool_requires_a11y_returns_error... ");

    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 1,
    };
    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    MdMcpToolCtx tool_ctx = { .agent = agent, .a11y = NULL };
    MdMcpServer *server = make_agent_mcp_server(&tool_ctx);

    const char *call = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":42,"
                       "\"params\":{\"name\":\"metadesk_click\","
                       "\"arguments\":{\"target_id\":\"btn_missing\"}}}";
    md_mcp_server_handle_message(server, call, strlen(call));
    assert(g_agent_mcp_response != NULL);

    cJSON *resp = cJSON_Parse(g_agent_mcp_response);
    assert(resp != NULL);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    assert(result != NULL);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
    cJSON *content = cJSON_GetObjectItem(result, "content");
    assert(cJSON_IsArray(content));
    const char *text = cJSON_GetObjectItem(cJSON_GetArrayItem(content, 0),
                                           "text")->valuestring;
    assert(strstr(text, "action execution failed") != NULL);
    cJSON_Delete(resp);

    assert(md_agent_get_action_count(agent) == 0);

    clear_agent_mcp_response();
    md_mcp_tools_cleanup(&tool_ctx);
    md_mcp_server_destroy(server);
    md_agent_destroy(agent);

    printf("OK\n");
    return 0;
}

/* ── Test: no action_count increment when a11y result is missing ─ */

static int test_action_count_not_incremented_when_a11y_null(void) {
    printf("  test_action_count_not_incremented_when_a11y_null... ");

    AgentMockInputData data = {0};
    MdInput input = make_agent_mock_input(&data);
    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = &input,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 1,
    };
    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    const char *json = "{\"v\":1,\"action\":\"click\","
                       "\"target_id\":\"btn_missing\",\"payload\":{}}";
    char *result = md_agent_handle_action_mcp(agent,
                                              (const uint8_t *)json,
                                              (uint32_t)strlen(json));
    assert(result == NULL);
    assert(md_agent_get_action_count(agent) == 0);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: handle_action with invalid payload ────────────────── */

static int test_handle_invalid_payload(void) {
    printf("  test_handle_invalid_payload... ");

    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 10,
    };
    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    /* Invalid JSON */
    const char *bad = "{{not json}}";
    int ret = md_agent_handle_action(agent, NULL, NULL,
                                     (const uint8_t *)bad,
                                     (uint32_t)strlen(bad));
    assert(ret == -1);

    /* NULL payload */
    ret = md_agent_handle_action(agent, NULL, NULL, NULL, 0);
    assert(ret == -1);

    /* Empty payload */
    ret = md_agent_handle_action(agent, NULL, NULL,
                                     (const uint8_t *)"", 0);
    assert(ret == -1);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: default settle_ms ─────────────────────────────────── */

static int test_default_settle(void) {
    printf("  test_default_settle... ");

    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 0,  /* should use default */
    };
    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    /* Just verify it was created — default settle_ms is internal */
    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: handle_action_mcp (MCP integration path) ──────────── */

static int test_handle_action_mcp(void) {
    printf("  test_handle_action_mcp... ");

    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 10,
    };
    MdAgent *agent = md_agent_create(&cfg);
    assert(agent != NULL);

    /* With no a11y result, MCP handler should return an error (NULL),
     * not a placeholder success response. */
    const char *json = "{\"v\":1,\"action\":\"click\",\"target_id\":\"btn1\","
                       "\"payload\":{}}";
    char *result = md_agent_handle_action_mcp(agent,
                                              (const uint8_t *)json,
                                              (uint32_t)strlen(json));
    assert(result == NULL);
    assert(md_agent_get_action_count(agent) == 0);

    /* NULL payload */
    result = md_agent_handle_action_mcp(agent, NULL, 0);
    assert(result == NULL);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_agent: agent action handler tests\n");

    int failures = 0;
    failures += test_create_destroy();
    failures += test_null_config();
    failures += test_handle_action_no_deps();
    failures += test_with_live_deps();
    failures += test_tree_format_config();
    failures += test_send_tree_delta_null_stream();
    failures += test_handle_various_actions();
    failures += test_injection_failure_propagates();
    failures += test_mcp_tool_requires_a11y_returns_error();
    failures += test_action_count_not_incremented_when_a11y_null();
    failures += test_handle_invalid_payload();
    failures += test_default_settle();
    failures += test_handle_action_mcp();

    clear_agent_mcp_response();
    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures;
}
