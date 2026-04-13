/*
 * metadesk — tests/test_input.c
 * Input injection tests.
 *
 * Note: Full uinput tests require /dev/uinput access (root or input group).
 * These tests validate the API surface and action dispatch logic.
 * Run with elevated permissions to test actual device creation.
 */
#include "input.h"
#include "action.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

/* ── Test: creation (may fail without permissions) ───────────── */

static int test_create_destroy(void) {
    printf("  test_create_destroy... ");

    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080 };
    MdInput *inp = md_input_create(&cfg);

    /* Creation should succeed even if /dev/uinput is unavailable
     * (it just won't be "ready") */
    assert(inp != NULL);

    if (md_input_is_ready(inp)) {
        printf("OK (devices created)\n");
    } else {
        printf("OK (no /dev/uinput access — skipping device tests)\n");
    }

    md_input_destroy(inp);
    return 0;
}

/* ── Test: create with NULL config uses defaults ─────────────── */

static int test_create_defaults(void) {
    printf("  test_create_defaults... ");

    MdInput *inp = md_input_create(NULL);
    assert(inp != NULL);
    md_input_destroy(inp);

    printf("OK\n");
    return 0;
}

/* ── Test: action dispatch with no devices ───────────────────── */

static int test_action_dispatch(void) {
    printf("  test_action_dispatch... ");

    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080 };
    MdInput *inp = md_input_create(&cfg);
    assert(inp != NULL);

    /* Build a click action */
    MdAction action;
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_CLICK;
    action.region[0] = 100;
    action.region[1] = 200;

    /* Execute — returns 0 if devices are ready, -1 otherwise */
    int ret = md_input_execute_action(inp, &action);
    if (md_input_is_ready(inp))
        assert(ret == 0);
    /* If not ready, -1 is expected */

    /* Key combo action */
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_KEY_COMBO;
    action.keys[0] = strdup("ctrl");
    action.keys[1] = strdup("s");
    action.key_count = 2;

    ret = md_input_execute_action(inp, &action);
    if (md_input_is_ready(inp))
        assert(ret == 0);

    md_action_cleanup(&action);

    /* Type text action */
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_TYPE;
    strncpy(action.text, "Hello, world!", sizeof(action.text) - 1);

    ret = md_input_execute_action(inp, &action);
    if (md_input_is_ready(inp))
        assert(ret == 0);

    /* Scroll action */
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_SCROLL;
    action.dx = 0;
    action.dy = 3;

    ret = md_input_execute_action(inp, &action);
    if (md_input_is_ready(inp))
        assert(ret == 0);

    /* Unknown action */
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_UNKNOWN;
    ret = md_input_execute_action(inp, &action);
    assert(ret == -1); /* should always fail */

    /* NULL action */
    ret = md_input_execute_action(inp, NULL);
    assert(ret == -1);

    md_input_destroy(inp);
    printf("OK\n");
    return 0;
}

/* ── Test: action from parsed JSON ───────────────────────────── */

static int test_action_from_json(void) {
    printf("  test_action_from_json... ");

    const char *json = "{\"v\":1,\"action\":\"key_combo\","
                       "\"payload\":{\"keys\":[\"ctrl\",\"shift\",\"t\"]}}";

    MdAction action;
    memset(&action, 0, sizeof(action));
    int ret = md_action_parse(&action, json, strlen(json));
    assert(ret == 0);
    assert(action.type == MD_ACTION_KEY_COMBO);
    assert(action.key_count == 3);
    assert(strcmp(action.keys[0], "ctrl") == 0);
    assert(strcmp(action.keys[1], "shift") == 0);
    assert(strcmp(action.keys[2], "t") == 0);

    /* Dispatch */
    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080 };
    MdInput *inp = md_input_create(&cfg);
    assert(inp != NULL);

    ret = md_input_execute_action(inp, &action);
    /* Only check success if devices are available */
    if (md_input_is_ready(inp))
        assert(ret == 0);

    md_action_cleanup(&action);
    md_input_destroy(inp);

    printf("OK\n");
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_input: uinput injection tests\n");

    if (access("/dev/uinput", W_OK) != 0) {
        printf("  NOTE: /dev/uinput not writable — device tests will be limited\n");
    }

    int failures = 0;
    failures += test_create_destroy();
    failures += test_create_defaults();
    failures += test_action_dispatch();
    failures += test_action_from_json();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures;
}
