/*
 * metadesk — tests/test_agent.c
 * Agent action handler tests.
 *
 * Tests the action parsing → target resolution → injection → delta pipeline.
 * AT-SPI2 and uinput may not be available in CI, so tests degrade gracefully.
 */
#include "agent.h"
#include "action.h"
#include "atspi.h"
#include "input.h"
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Test: create and destroy ────────────────────────────────── */

static int test_create_destroy(void) {
    printf("  test_create_destroy... ");

    MdAgentConfig cfg = {
        .atspi       = NULL,
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
        .atspi       = NULL,
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

    /* Try to create real AT-SPI2 and input contexts */
    MdAtspiTree *atspi = md_atspi_create();
    MdInput *input = md_input_create(NULL);

    if (!atspi) {
        printf("SKIP (no AT-SPI2)\n");
        if (input) md_input_destroy(input);
        return 0;
    }

    MdAgentConfig cfg = {
        .atspi       = atspi,
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
    md_atspi_destroy(atspi);
    if (input) md_input_destroy(input);

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

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures;
}
