/*
 * metadesk — tests/test_agent.c
 * Agent action handler tests.
 *
 * Tests the action parsing → target resolution → injection → delta pipeline.
 * Live accessibility requires explicit opt-in; unit tests never create input devices.
 */
#include "agent.h"
#include "action.h"
#include "a11y.h"
#include "input.h"
#include "session.h"
#include "mcp_server.h"
#include "md_mcp_fixture.h"
#include "mcp_tools.h"
#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md_test.h"

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
    MD_CHECK(server != NULL);
    MD_CHECK(md_mcp_register_tools(server, tool_ctx) == 0);

    md_test_mcp_handshake(server);
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
    MD_CHECK(agent != NULL);
    MD_CHECK(md_agent_get_action_count(agent) == 0);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: NULL config should fail ───────────────────────────── */

static int test_null_config(void) {
    printf("  test_null_config... ");
    MD_CHECK(md_agent_create(NULL) == NULL);
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
    MD_CHECK(agent != NULL);

    /* We can't send packets without a real stream, but we can verify
     * the agent handles NULL stream gracefully */
    const char *json = "{\"v\":1,\"action\":\"click\",\"target_id\":\"n42\","
                       "\"payload\":{}}";
    int ret = md_agent_handle_action(agent, NULL, NULL,
                                     (const uint8_t *)json, (uint32_t)strlen(json));
    MD_CHECK(ret == -1); /* should fail because stream is NULL */

    MD_CHECK(md_agent_get_action_count(agent) == 0);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: action count tracking ─────────────────────────────── */

static int test_with_live_a11y(void) {
    printf("  test_with_live_a11y... ");

    const char *live = getenv("MD_A11Y_LIVE_TESTS");
    if (!live || strcmp(live, "1")) {
        printf("SKIP (requires explicit live accessibility opt-in)\n");
        return 0;
    }

    /* This tree-only check never needs a platform input device. */
    MdA11yCtx *a11y = md_a11y_create();

    if (!a11y) {
        printf("SKIP (no accessibility bus)\n");
        return 0;
    }

    MdAgentConfig cfg = {
        .a11y        = a11y,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_COMPACT,
        .settle_ms   = 10,
    };

    MdAgent *agent = md_agent_create(&cfg);
    MD_CHECK(agent != NULL);

    /* Verify send_tree works with live AT-SPI2 but NULL stream
     * should return -1 (no stream) */
    uint32_t seq = 0;
    int ret = md_agent_send_tree(agent, NULL, &seq);
    MD_CHECK(ret == -1);

    md_agent_destroy(agent);
    md_a11y_destroy(a11y);

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
    MD_CHECK(a1 != NULL);
    md_agent_destroy(a1);

    /* Compact format */
    MdAgentConfig cfg2 = {
        .a11y        = NULL,
        .input       = NULL,
        .tree_format = MD_TREE_FORMAT_COMPACT,
        .settle_ms   = 50,
    };
    MdAgent *a2 = md_agent_create(&cfg2);
    MD_CHECK(a2 != NULL);
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
    MD_CHECK(agent != NULL);

    uint32_t seq = 0;

    /* send_tree with NULL stream should fail gracefully */
    int ret = md_agent_send_tree(agent, NULL, &seq);
    MD_CHECK(ret == -1);

    /* send_delta with NULL stream should fail gracefully */
    ret = md_agent_send_delta(agent, NULL, &seq);
    MD_CHECK(ret == -1);

    /* seq should not have been incremented */
    MD_CHECK(seq == 0);

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
    MD_CHECK(agent != NULL);

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
        MD_CHECK(ret == -1); /* no stream → fail */
    }

    /* Action count should still be 0 (none completed successfully) */
    MD_CHECK(md_agent_get_action_count(agent) == 0);

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
    MD_CHECK(agent != NULL);

    const char *json = "{\"v\":1,\"action\":\"type\","
                       "\"payload\":{\"text\":\"hello\"}}";
    uint32_t seq = 7;
    int ret = md_agent_handle_action(agent, (MdStream *)0x1, &seq,
                                     (const uint8_t *)json,
                                     (uint32_t)strlen(json));
    MD_CHECK(ret == -1);
    MD_CHECK(seq == 7);
    MD_CHECK(data.type_text_calls == 1);
    MD_CHECK(md_agent_get_action_count(agent) == 0);
    md_agent_destroy(agent);

    data = (AgentMockInputData){ .type_text_ret = -1 };
    input = make_agent_mock_input(&data);
    cfg.input = &input;
    agent = md_agent_create(&cfg);
    MD_CHECK(agent != NULL);

    char *result = md_agent_handle_action_mcp(agent,
                                              (const uint8_t *)json,
                                              (uint32_t)strlen(json));
    MD_CHECK(result == NULL);
    MD_CHECK(data.type_text_calls == 1);
    MD_CHECK(md_agent_get_action_count(agent) == 0);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* ── Test: MCP tool failure when a11y is unavailable ─────────── */

static int test_mcp_tool_requires_a11y_returns_error(void) {
    printf("  test_mcp_tool_requires_a11y_returns_error... ");

    AgentMockInputData data = {0};
    MdInput input = make_agent_mock_input(&data);
    MdAgentConfig cfg = {
        .a11y        = NULL,
        .input       = &input,
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms   = 1,
    };
    MdAgent *agent = md_agent_create(&cfg);
    MD_CHECK(agent != NULL);

    MdMcpToolCtx tool_ctx = { .agent = agent, .input = &input };
    MdMcpServer *server = make_agent_mcp_server(&tool_ctx);

    const char *call = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":42,"
                       "\"params\":{\"name\":\"metadesk_click\","
                       "\"arguments\":{\"target_id\":\"btn_missing\"}}}";
    md_mcp_server_handle_message(server, call, strlen(call));
    MD_CHECK(g_agent_mcp_response != NULL);

    cJSON *resp = cJSON_Parse(g_agent_mcp_response);
    MD_CHECK(resp != NULL);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    MD_CHECK(result != NULL);
    MD_CHECK(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
    cJSON *content = cJSON_GetObjectItem(result, "content");
    MD_CHECK(cJSON_IsArray(content));
    const char *text = cJSON_GetObjectItem(cJSON_GetArrayItem(content, 0),
                                           "text")->valuestring;
    MD_CHECK(strstr(text, "action execution failed") != NULL);
    cJSON_Delete(resp);

    MD_CHECK(md_agent_get_action_count(agent) == 0);

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
    MD_CHECK(agent != NULL);

    const char *json = "{\"v\":1,\"action\":\"click\","
                       "\"target_id\":\"btn_missing\",\"payload\":{}}";
    char *result = md_agent_handle_action_mcp(agent,
                                              (const uint8_t *)json,
                                              (uint32_t)strlen(json));
    MD_CHECK(result == NULL);
    MD_CHECK(md_agent_get_action_count(agent) == 0);

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
    MD_CHECK(agent != NULL);

    /* Invalid JSON */
    const char *bad = "{{not json}}";
    int ret = md_agent_handle_action(agent, NULL, NULL,
                                     (const uint8_t *)bad,
                                     (uint32_t)strlen(bad));
    MD_CHECK(ret == -1);

    /* NULL payload */
    ret = md_agent_handle_action(agent, NULL, NULL, NULL, 0);
    MD_CHECK(ret == -1);

    /* Empty payload */
    ret = md_agent_handle_action(agent, NULL, NULL,
                                     (const uint8_t *)"", 0);
    MD_CHECK(ret == -1);

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
    MD_CHECK(agent != NULL);

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
    MD_CHECK(agent != NULL);

    /* With no a11y result, MCP handler should return an error (NULL),
     * not a placeholder success response. */
    const char *json = "{\"v\":1,\"action\":\"click\",\"target_id\":\"btn1\","
                       "\"payload\":{}}";
    char *result = md_agent_handle_action_mcp(agent,
                                              (const uint8_t *)json,
                                              (uint32_t)strlen(json));
    MD_CHECK(result == NULL);
    MD_CHECK(md_agent_get_action_count(agent) == 0);

    /* NULL payload */
    result = md_agent_handle_action_mcp(agent, NULL, 0);
    MD_CHECK(result == NULL);

    md_agent_destroy(agent);
    printf("OK\n");
    return 0;
}

/* Deterministic tree changes without a platform accessibility service. */
static int agent_fake_tree(MdA11yCtx *ctx, MdA11yNode **out) {
    int *walks = ctx->backend_data;
    if (*walks < 0) return -1;
    MdA11yNode *node = calloc(1, sizeof(*node));
    node->id = strdup("btn1");
    node->role = strdup("button");
    node->label = strdup((*walks)++ == 0 ? "Before" : "After");
    *out = node;
    return 0;
}

static int test_missing_input_never_acknowledges(void) {
    printf("  test_missing_input_never_acknowledges... ");
    const MdA11yBackend backend = { .get_tree = agent_fake_tree };
    AgentMockInputData data = {0};
    MdInput input = make_agent_mock_input(&data);
    input.ready = false;
    for (int i = 0; i < 2; i++) {
        int walks = 0;
        MdA11yCtx a11y = { .vtable = &backend, .backend_data = &walks };
        MD_CHECK(pthread_mutex_init(&a11y.snapshot_mu, NULL) == 0);
        MdAgentConfig cfg = {
            .a11y = &a11y, .input = i ? &input : NULL,
            .tree_format = MD_TREE_FORMAT_JSON, .settle_ms = 1,
        };
        MdAgent *agent = md_agent_create(&cfg);
        MD_CHECK(agent != NULL);
        const char action[] = "{\"v\":1,\"action\":\"type\",\"payload\":{\"text\":\"hello\"}}";
        char *result = md_agent_handle_action_mcp(agent,
            (const uint8_t *)action, sizeof(action) - 1);
        MD_CHECK(result == NULL);
        free(result);
        MD_CHECK(md_agent_get_action_count(agent) == 0);
        MD_CHECK(data.type_text_calls == 0);
        MD_CHECK(walks == 0);
        md_agent_destroy(agent);
        pthread_mutex_destroy(&a11y.snapshot_mu);
    }
    printf("OK\n");
    return 0;
}

static int test_format_aware_updates(void) {
    printf("  test_format_aware_updates... ");
    const MdA11yBackend backend = { .get_tree = agent_fake_tree };
    MdStreamServer *server = md_stream_server_create(NULL, 0);
    MD_CHECK(server != NULL);
    if (!server) return 0;
    uint16_t port = md_stream_server_port(server);
    MD_CHECK(port != 0);
    if (!port) {
        md_stream_server_destroy(server);
        return 0;
    }
    MdStream *client = md_stream_connect("127.0.0.1", port, 3000);
    MD_CHECK(client != NULL);
    if (!client) {
        md_stream_server_destroy(server);
        return 0;
    }
    MdStream *host = md_stream_server_accept(server, 3000);
    MD_CHECK(host != NULL);
    if (!host) {
        md_stream_destroy(client);
        md_stream_server_destroy(server);
        return 0;
    }

    for (int compact = 0; compact <= 1; compact++) {
        int walks = 0;
        MdA11yCtx a11y = { .vtable = &backend, .backend_data = &walks };
        MD_CHECK(pthread_mutex_init(&a11y.snapshot_mu, NULL) == 0);
        int count = 0;
        MdA11yDelta *initial = md_a11y_diff(&a11y, &count);
        md_a11y_delta_free(initial, count);
        AgentMockInputData data = {0};
        MdInput input = make_agent_mock_input(&data);
        MdAgentConfig cfg = {
            .a11y = &a11y,
            .input = &input,
            .tree_format = compact ? MD_TREE_FORMAT_COMPACT : MD_TREE_FORMAT_JSON,
            .settle_ms = 1,
        };
        MdAgent *agent = md_agent_create(&cfg);
        MD_CHECK(agent != NULL);
        uint32_t seq = 0;
        const char action[] = "{\"v\":1,\"action\":\"click\",\"payload\":{}}";
        MD_CHECK(md_agent_handle_action(agent, host, &seq,
                  (const uint8_t *)action, sizeof(action) - 1) == 0);
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        MD_CHECK(md_stream_recv(client, &hdr, &payload, 3000) == 0);
        MD_CHECK(hdr.type == (compact ? MD_PKT_UI_TREE : MD_PKT_UI_TREE_DELTA));
        if (compact) {
            MD_CHECK(hdr.payload_len >= 6 && memcmp(payload, "v1 ts:", 6) == 0);
        } else {
            cJSON *delta = cJSON_ParseWithLength((const char *)payload, hdr.payload_len);
            MD_CHECK(cJSON_IsArray(delta) && cJSON_GetArraySize(delta) == 1);
            cJSON_Delete(delta);
        }
        free(payload);
        if (compact) {
            MD_CHECK(md_agent_send_delta(agent, host, &seq) == 0);
            MD_CHECK(md_stream_recv(client, &hdr, &payload, 3000) == 0);
            MD_CHECK(hdr.type == MD_PKT_UI_TREE);
            MD_CHECK(hdr.payload_len >= 6 && memcmp(payload, "v1 ts:", 6) == 0);
            free(payload);
        }
        walks = -1; /* unavailable tree is a no-op for both update formats */
        uint32_t previous_seq = seq;
        MD_CHECK(md_agent_send_delta(agent, host, &seq) == 0);
        MD_CHECK(seq == previous_seq);
        MD_CHECK(md_agent_send_tree(agent, host, &seq) == -1);
        md_agent_destroy(agent);
        md_a11y_node_free(a11y.last_snapshot);
        pthread_mutex_destroy(&a11y.snapshot_mu);
    }
    md_stream_destroy(host);
    md_stream_destroy(client);
    md_stream_server_destroy(server);
    printf("OK\n");
    return 0;
}

typedef struct {
    const char *target, *front;
    int hit_calls, target_clicks, occluder_clicks;
} HitFixture;

static int hit_fixture_tree(MdA11yCtx *ctx, MdA11yNode **out) {
    HitFixture *f = ctx->backend_data;
    MdA11yNode *node = calloc(1, sizeof(*node));
    node->id = strdup(f->target);
    node->role = strdup("button");
    node->label = strdup("Owned target");
    node->x = 100; node->y = 200; node->w = 80; node->h = 40;
    *out = node;
    return 0;
}

static char *hit_fixture_point(MdA11yCtx *ctx, int x, int y) {
    HitFixture *f = ctx->backend_data;
    MD_CHECK(x == 140 && y == 220);
    f->hit_calls++;
    return f->front ? strdup(f->front) : NULL;
}

static int hit_fixture_move(MdInputCtx *ctx, int x, int y) {
    (void)ctx;
    MD_CHECK(x == 140 && y == 220);
    return 0;
}
static int hit_fixture_button(MdInputCtx *ctx, int button, int pressed) {
    HitFixture *f = ctx->backend_data;
    (void)button;
    if (!pressed) {
        if (strcmp(f->front, f->target) == 0) f->target_clicks++;
        else f->occluder_clicks++;
    }
    return 0;
}

static int test_target_hit_verification(void) {
    const char *ids[] = {"ax123:abc", "atspi::1.23:/org/a11y/atspi/accessible/42", "u:2a.101b0.4"};
    const char *actions[] = {"click", "dbl_click", "right_click", "focus"};
    const MdInputBackend input_backend = { .mouse_move = hit_fixture_move,
                                            .mouse_button = hit_fixture_button };
    for (size_t platform = 0; platform < 3; platform++) {
        for (size_t action = 0; action < 4; action++) {
            HitFixture f = { .target = ids[platform], .front = "owned-occluder" };
            MdA11yBackend backend = { .get_tree = hit_fixture_tree, .hit_test = hit_fixture_point };
            MdA11yCtx a11y = { .vtable = &backend, .backend_data = &f,
                               .snapshot_mu = PTHREAD_MUTEX_INITIALIZER };
            MdInput input = { .vtable = &input_backend, .backend_data = &f, .ready = true };
            MdAgentConfig cfg = { .a11y = &a11y, .input = &input,
                                  .tree_format = MD_TREE_FORMAT_COMPACT, .settle_ms = 1 };
            MdAgent *agent = md_agent_create(&cfg);
            char json[512];
            snprintf(json, sizeof(json), "{\"v\":1,\"action\":\"%s\",\"target_id\":\"%s\",\"payload\":{}}",
                     actions[action], f.target);
            /* Occluded and unsupported hit tests must not inject at all. */
            char *result = md_agent_handle_action_mcp(agent, (const uint8_t *)json, strlen(json));
            MD_CHECK(!result && f.target_clicks == 0 && f.occluder_clicks == 0);
            MD_CHECK(md_agent_get_action_count(agent) == 0);
            free(result);
            f.front = NULL;
            result = md_agent_handle_action_mcp(agent, (const uint8_t *)json, strlen(json));
            MD_CHECK(!result && f.target_clicks == 0 && f.occluder_clicks == 0);
            free(result);
            backend.hit_test = NULL;
            result = md_agent_handle_action_mcp(agent, (const uint8_t *)json, strlen(json));
            MD_CHECK(!result && f.target_clicks == 0 && f.occluder_clicks == 0);
            free(result);
            backend.hit_test = hit_fixture_point;
            f.front = f.target;
            result = md_agent_handle_action_mcp(agent, (const uint8_t *)json, strlen(json));
            MD_CHECK(result && f.target_clicks == (action == 1 ? 2 : 1));
            MD_CHECK(f.occluder_clicks == 0 && md_agent_get_action_count(agent) == 1);
            free(result);
            md_agent_destroy(agent);
            md_a11y_node_free(a11y.last_snapshot);
            pthread_mutex_destroy(&a11y.snapshot_mu);
        }
    }
    puts("  PASS: AX/AT-SPI/UIA identity fakes reject occluders, allow actual hits");
    return 0;
}

/* A tree walk held on an event, not a guessed sleep, must not hold the
 * transport. Both JSON and compact must still return a complete tree. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool entered, release;
} BlockedTree;

static int agent_blocked_tree(MdA11yCtx *ctx, MdA11yNode **out) {
    BlockedTree *state = ctx->backend_data;
    pthread_mutex_lock(&state->mu);
    state->entered = true;
    pthread_cond_broadcast(&state->cv);
    while (!state->release) pthread_cond_wait(&state->cv, &state->mu);
    pthread_mutex_unlock(&state->mu);
    int walks = 0;
    MdA11yCtx fake = { .backend_data = &walks };
    return agent_fake_tree(&fake, out);
}

static int test_initial_tree_does_not_block_video(void) {
    const MdA11yBackend backend = { .get_tree = agent_blocked_tree };
    MdStreamServer *server = md_stream_server_create(NULL, 0);
    MD_CHECK(server);
    MdStream *client = md_stream_connect("127.0.0.1",
                                         md_stream_server_port(server), 3000);
    MdStream *host = md_stream_server_accept(server, 3000);
    MD_CHECK(client && host);
    for (int compact = 0; compact < 2; compact++) {
        BlockedTree state = { .mu = PTHREAD_MUTEX_INITIALIZER,
                              .cv = PTHREAD_COND_INITIALIZER };
        MdA11yCtx a11y = { .vtable = &backend, .backend_data = &state };
        MdAgentConfig cfg = { .a11y = &a11y,
            .tree_format = compact ? MD_TREE_FORMAT_COMPACT : MD_TREE_FORMAT_JSON };
        MdAgent *agent = md_agent_create(&cfg);
        MdAgentTreeJob *job = md_agent_tree_start(agent);
        MD_CHECK(job);
        pthread_mutex_lock(&state.mu);
        while (!state.entered) pthread_cond_wait(&state.cv, &state.mu);
        pthread_mutex_unlock(&state.mu);
        char *tree = NULL;
        MD_CHECK(md_agent_tree_take(job, &tree) == 0 && !tree);
        /* A frame is delivered while the accessibility provider is blocked. */
        const uint8_t frame[] = {1, 2, 3};
        MD_CHECK(md_stream_send(host, MD_PKT_VIDEO_FRAME, 1, frame, sizeof(frame)) == 0);
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        MD_CHECK(md_stream_recv(client, &hdr, &payload, 3000) == 0);
        MD_CHECK(hdr.type == MD_PKT_VIDEO_FRAME && hdr.payload_len == sizeof(frame));
        MD_CHECK(payload && memcmp(payload, frame, sizeof(frame)) == 0);
        free(payload);
        pthread_mutex_lock(&state.mu);
        state.release = true;
        pthread_cond_broadcast(&state.cv);
        pthread_mutex_unlock(&state.mu);
        tree = md_agent_tree_finish(job);
        MD_CHECK(tree && strstr(tree, "btn1") && strstr(tree, "Before"));
        free(tree);
        md_agent_destroy(agent);
        pthread_cond_destroy(&state.cv);
        pthread_mutex_destroy(&state.mu);
    }
    md_stream_destroy(host);
    md_stream_destroy(client);
    md_stream_server_destroy(server);
    puts("  PASS: initial tree worker preserves video and complete snapshots");
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_agent: agent action handler tests\n");

    int failures = 0;
    failures += test_create_destroy();
    failures += test_null_config();
    failures += test_handle_action_no_deps();
    failures += test_with_live_a11y();
    failures += test_tree_format_config();
    failures += test_send_tree_delta_null_stream();
    failures += test_handle_various_actions();
    failures += test_injection_failure_propagates();
    failures += test_mcp_tool_requires_a11y_returns_error();
    failures += test_action_count_not_incremented_when_a11y_null();
    failures += test_handle_invalid_payload();
    failures += test_default_settle();
    failures += test_handle_action_mcp();
    failures += test_missing_input_never_acknowledges();
    failures += test_format_aware_updates();
    failures += test_initial_tree_does_not_block_video();
    failures += test_target_hit_verification();

    clear_agent_mcp_response();
    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    md_test_failures += failures;
    return md_test_report();
}
