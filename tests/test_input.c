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

/* ── Test: keysym lookup from name ────────────────────────────── */

static int test_keysym_lookup(void) {
    printf("  test_keysym_lookup... ");

    /* Modifiers */
    assert(md_input_keysym_from_name("ctrl")  == 0x001D);
    assert(md_input_keysym_from_name("control") == 0x001D);
    assert(md_input_keysym_from_name("shift") == 0x002A);
    assert(md_input_keysym_from_name("alt")   == 0x0038);
    assert(md_input_keysym_from_name("super") == 0x007D);
    assert(md_input_keysym_from_name("meta")  == 0x007D);
    assert(md_input_keysym_from_name("win")   == 0x007D);

    /* Special keys */
    assert(md_input_keysym_from_name("enter")     == 0x001C);
    assert(md_input_keysym_from_name("return")    == 0x001C);
    assert(md_input_keysym_from_name("tab")       == 0x000F);
    assert(md_input_keysym_from_name("escape")    == 0x0001);
    assert(md_input_keysym_from_name("esc")       == 0x0001);
    assert(md_input_keysym_from_name("backspace") == 0x000E);
    assert(md_input_keysym_from_name("delete")    == 0x006F);
    assert(md_input_keysym_from_name("space")     == 0x0039);

    /* Navigation */
    assert(md_input_keysym_from_name("up")       == 0x0067);
    assert(md_input_keysym_from_name("down")     == 0x006C);
    assert(md_input_keysym_from_name("left")     == 0x0069);
    assert(md_input_keysym_from_name("right")    == 0x006A);
    assert(md_input_keysym_from_name("home")     == 0x0066);
    assert(md_input_keysym_from_name("end")      == 0x006B);
    assert(md_input_keysym_from_name("pageup")   == 0x0068);
    assert(md_input_keysym_from_name("pagedown") == 0x006D);

    /* F-keys */
    assert(md_input_keysym_from_name("f1")  == 0x003B);
    assert(md_input_keysym_from_name("f12") == 0x0058);

    /* Letters */
    assert(md_input_keysym_from_name("a") == 0x001E);
    assert(md_input_keysym_from_name("z") == 0x002C);
    assert(md_input_keysym_from_name("s") == 0x001F);

    /* Digits */
    assert(md_input_keysym_from_name("0") == 0x000B);
    assert(md_input_keysym_from_name("1") == 0x0002);
    assert(md_input_keysym_from_name("9") == 0x000A);

    /* Punctuation */
    assert(md_input_keysym_from_name("-")  == 0x000C);
    assert(md_input_keysym_from_name("=")  == 0x000D);
    assert(md_input_keysym_from_name(",")  == 0x0033);
    assert(md_input_keysym_from_name(".")  == 0x0034);
    assert(md_input_keysym_from_name("/")  == 0x0035);

    /* Case insensitivity */
    assert(md_input_keysym_from_name("CTRL")  == 0x001D);
    assert(md_input_keysym_from_name("Shift") == 0x002A);
    assert(md_input_keysym_from_name("F1")    == 0x003B);

    /* Unknown → 0 */
    assert(md_input_keysym_from_name("nonexistent") == 0);
    assert(md_input_keysym_from_name(NULL)          == 0);
    assert(md_input_keysym_from_name("")            == 0);

    printf("OK\n");
    return 0;
}

/* ── Test: dbl_click and right_click ─────────────────────────── */

static int test_dbl_and_right_click(void) {
    printf("  test_dbl_and_right_click... ");

    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080 };
    MdInput *inp = md_input_create(&cfg);
    assert(inp != NULL);

    /* These call through the backend — verify they don't crash
     * regardless of device availability */
    int ret;

    ret = md_input_dbl_click(inp, 500, 300);
    if (md_input_is_ready(inp)) assert(ret == 0);

    ret = md_input_right_click(inp, 500, 300);
    if (md_input_is_ready(inp)) assert(ret == 0);

    /* NULL input */
    assert(md_input_dbl_click(NULL, 0, 0) == -1);
    assert(md_input_right_click(NULL, 0, 0) == -1);

    md_input_destroy(inp);
    printf("OK\n");
    return 0;
}

/* ── Test: mouse_move and scroll ─────────────────────────────── */

static int test_mouse_scroll(void) {
    printf("  test_mouse_scroll... ");

    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080 };
    MdInput *inp = md_input_create(&cfg);
    assert(inp != NULL);

    /* scroll with NULL */
    assert(md_input_scroll(NULL, 0, 3) == -1);

    /* mouse_move with NULL */
    assert(md_input_mouse_move(NULL, 100, 100) == -1);

    /* Normal calls (may or may not succeed depending on device) */
    int ret = md_input_scroll(inp, 0, 3);
    if (md_input_is_ready(inp)) assert(ret == 0);

    ret = md_input_mouse_move(inp, 100, 200);
    if (md_input_is_ready(inp)) assert(ret == 0);

    md_input_destroy(inp);
    printf("OK\n");
    return 0;
}

/* ── Test: type_text and key_combo edge cases ────────────────── */

static int test_type_key_combo_edges(void) {
    printf("  test_type_key_combo_edges... ");

    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080 };
    MdInput *inp = md_input_create(&cfg);
    assert(inp != NULL);

    /* NULL text */
    assert(md_input_type_text(inp, NULL) == -1);
    assert(md_input_type_text(NULL, "hello") == -1);

    /* NULL keys array */
    assert(md_input_key_combo(inp, NULL, 1) == -1);

    /* Zero key count */
    const char *keys[] = {"ctrl"};
    assert(md_input_key_combo(inp, keys, 0) == -1);

    /* NULL input */
    assert(md_input_key_combo(NULL, keys, 1) == -1);

    /* Unknown key name should fail */
    const char *bad_keys[] = {"nonexistent_key"};
    assert(md_input_key_combo(inp, bad_keys, 1) == -1);

    md_input_destroy(inp);
    printf("OK\n");
    return 0;
}

/* ── Test: is_ready and destroy NULL ─────────────────────────── */

static int test_lifecycle_edges(void) {
    printf("  test_lifecycle_edges... ");

    assert(md_input_is_ready(NULL) == false);
    md_input_destroy(NULL); /* should not crash */

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
    failures += test_keysym_lookup();
    failures += test_dbl_and_right_click();
    failures += test_mouse_scroll();
    failures += test_type_key_combo_edges();
    failures += test_lifecycle_edges();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures;
}
