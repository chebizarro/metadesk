/*
 * metadesk — tests/test_input.c
 * Input injection tests.
 *
 * Default runs use mock/wire-level peers, never desktop input devices.
 * MD_INPUT_LIVE_TESTS=1 explicitly opts into platform device creation; use
 * that only on a disposable desktop. Permissions alone are not consent.
 */
#include "input.h"
#include "action.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "md_test.h"

/* ── Mock backend for deterministic HAL error propagation tests ─ */

typedef struct {
    int mouse_move_calls;
    int mouse_button_calls;
    int key_event_calls;
    int mouse_scroll_calls;
    int type_text_calls;

    int mouse_move_ret;
    int mouse_button_fail_call; /* 1-based call index, 0 = never */
    int key_event_fail_call;    /* 1-based call index, 0 = never */

    int last_x;
    int last_y;
    int last_dx;
    int last_dy;
    char last_text[128];
    int button_pressed[4];
    uint32_t keysyms[16];
    int key_pressed[16];
} MockInputData;

static int mock_mouse_move(MdInputCtx *ctx, int x, int y) {
    MockInputData *data = (MockInputData *)ctx->backend_data;
    data->mouse_move_calls++;
    data->last_x = x;
    data->last_y = y;
    return data->mouse_move_ret;
}

static int mock_mouse_button(MdInputCtx *ctx, int button, int pressed) {
    MockInputData *data = (MockInputData *)ctx->backend_data;
    data->mouse_button_calls++;
    if (data->mouse_button_calls == data->mouse_button_fail_call)
        return -1;
    if (button >= 0 && button < 4)
        data->button_pressed[button] = pressed;
    return 0;
}

static int mock_key_event(MdInputCtx *ctx, uint32_t keysym, int pressed) {
    MockInputData *data = (MockInputData *)ctx->backend_data;
    int idx = data->key_event_calls++;
    if (idx < 16) {
        data->keysyms[idx] = keysym;
        data->key_pressed[idx] = pressed;
    }
    if (data->key_event_calls == data->key_event_fail_call)
        return -1;
    return 0;
}

static int mock_mouse_scroll(MdInputCtx *ctx, int dx, int dy) {
    MockInputData *data = (MockInputData *)ctx->backend_data;
    data->mouse_scroll_calls++;
    data->last_dx = dx;
    data->last_dy = dy;
    return 0;
}

static int mock_type_text(MdInputCtx *ctx, const char *utf8) {
    MockInputData *data = (MockInputData *)ctx->backend_data;
    data->type_text_calls++;
    snprintf(data->last_text, sizeof(data->last_text), "%s",
             utf8 ? utf8 : "");
    return 0;
}

static const MdInputBackend mock_backend = {
    .mouse_move = mock_mouse_move,
    .mouse_button = mock_mouse_button,
    .mouse_scroll = mock_mouse_scroll,
    .key_event = mock_key_event,
    .type_text = mock_type_text,
};

static MdInput make_mock_input(MockInputData *data,
                               const MdInputBackend *backend) {
    MdInput inp;
    memset(&inp, 0, sizeof(inp));
    inp.vtable = backend;
    inp.backend_data = data;
    inp.ready = true;
    return inp;
}

/* ── Test: creation (may fail without permissions) ───────────── */

static int test_create_destroy(void) {
    printf("  test_create_destroy... ");
    if (!getenv("MD_INPUT_LIVE_TESTS") || strcmp(getenv("MD_INPUT_LIVE_TESTS"), "1")) {
        puts("SKIP (requires explicit disposable-desktop opt-in)");
        return 0;
    }

    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080 };
    MdInput *inp = md_input_create(&cfg);

    /* Explicit live test: the platform backend must initialize. */
    MD_CHECK(inp != NULL);

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
    if (!getenv("MD_INPUT_LIVE_TESTS") || strcmp(getenv("MD_INPUT_LIVE_TESTS"), "1")) {
        puts("SKIP (requires explicit disposable-desktop opt-in)");
        return 0;
    }

    MdInput *inp = md_input_create(NULL);
    MD_CHECK(inp != NULL);
    md_input_destroy(inp);

    printf("OK\n");
    return 0;
}

/* ── Test: action dispatch against the mock backend ────────────
 * Deterministic: asserts the exact injected event sequence for every
 * action class, independent of device availability. */

static int test_action_dispatch(void) {
    printf("  test_action_dispatch... ");

    MockInputData data = {0};
    MdInput inp = make_mock_input(&data, &mock_backend);

    /* Click: move to (100,200), press, release */
    MdAction action;
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_CLICK;
    action.region[0] = 100;
    action.region[1] = 200;
    MD_CHECK(md_input_execute_action(&inp, &action) == 0);
    MD_CHECK(data.mouse_move_calls == 1);
    MD_CHECK(data.last_x == 100 && data.last_y == 200);
    MD_CHECK(data.mouse_button_calls == 2);
    MD_CHECK(data.button_pressed[MD_MOUSE_LEFT] == 0);

    /* Key combo: press ctrl, press s, release s, release ctrl */
    data = (MockInputData){0};
    inp = make_mock_input(&data, &mock_backend);
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_KEY_COMBO;
    action.keys[0] = strdup("ctrl");
    action.keys[1] = strdup("s");
    action.key_count = 2;
    MD_CHECK(md_input_execute_action(&inp, &action) == 0);
    MD_CHECK(data.key_event_calls == 4);
    MD_CHECK(data.keysyms[0] == 0x001D && data.key_pressed[0] == 1);
    MD_CHECK(data.keysyms[1] == 0x001F && data.key_pressed[1] == 1);
    MD_CHECK(data.keysyms[2] == 0x001F && data.key_pressed[2] == 0);
    MD_CHECK(data.keysyms[3] == 0x001D && data.key_pressed[3] == 0);
    md_action_cleanup(&action);

    /* Type text */
    data = (MockInputData){0};
    inp = make_mock_input(&data, &mock_backend);
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_TYPE;
    strncpy(action.text, "Hello, world!", sizeof(action.text) - 1);
    MD_CHECK(md_input_execute_action(&inp, &action) == 0);
    MD_CHECK(data.type_text_calls == 1);
    MD_CHECK(strcmp(data.last_text, "Hello, world!") == 0);

    /* Scroll */
    data = (MockInputData){0};
    inp = make_mock_input(&data, &mock_backend);
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_SCROLL;
    action.dx = 0;
    action.dy = 3;
    MD_CHECK(md_input_execute_action(&inp, &action) == 0);
    MD_CHECK(data.mouse_scroll_calls == 1);
    MD_CHECK(data.last_dx == 0 && data.last_dy == 3);

    /* set_value: select-all (platform accel key) then type */
    data = (MockInputData){0};
    inp = make_mock_input(&data, &mock_backend);
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_SET_VALUE;
    strncpy(action.text, "replacement", sizeof(action.text) - 1);
    MD_CHECK(md_input_execute_action(&inp, &action) == 0);
    MD_CHECK(data.key_event_calls == 4);
#ifdef __APPLE__
    MD_CHECK(data.keysyms[0] == 0x007D); /* meta/Cmd on macOS */
#else
    MD_CHECK(data.keysyms[0] == 0x001D); /* ctrl elsewhere */
#endif
    MD_CHECK(data.keysyms[1] == 0x001E); /* a */
    MD_CHECK(data.type_text_calls == 1);
    MD_CHECK(strcmp(data.last_text, "replacement") == 0);

    /* Unknown action always fails */
    data = (MockInputData){0};
    inp = make_mock_input(&data, &mock_backend);
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_UNKNOWN;
    MD_CHECK(md_input_execute_action(&inp, &action) == -1);

    /* NULL action */
    MD_CHECK(md_input_execute_action(&inp, NULL) == -1);

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
    MD_CHECK(ret == 0);
    MD_CHECK(action.type == MD_ACTION_KEY_COMBO);
    MD_CHECK(action.key_count == 3);
    MD_CHECK(strcmp(action.keys[0], "ctrl") == 0);
    MD_CHECK(strcmp(action.keys[1], "shift") == 0);
    MD_CHECK(strcmp(action.keys[2], "t") == 0);

    /* Dispatch through the mock backend — deterministic */
    MockInputData data = {0};
    MdInput inp = make_mock_input(&data, &mock_backend);

    ret = md_input_execute_action(&inp, &action);
    MD_CHECK(ret == 0);
    MD_CHECK(data.key_event_calls == 6); /* 3 presses + 3 releases */
    MD_CHECK(data.keysyms[0] == 0x001D); /* ctrl */
    MD_CHECK(data.keysyms[1] == 0x002A); /* shift */
    MD_CHECK(data.keysyms[2] == 0x0014); /* t */

    md_action_cleanup(&action);

    printf("OK\n");
    return 0;
}

/* ── Test: keysym lookup from name ────────────────────────────── */

static int test_keysym_lookup(void) {
    printf("  test_keysym_lookup... ");

    /* Modifiers */
    MD_CHECK(md_input_keysym_from_name("ctrl")  == 0x001D);
    MD_CHECK(md_input_keysym_from_name("control") == 0x001D);
    MD_CHECK(md_input_keysym_from_name("shift") == 0x002A);
    MD_CHECK(md_input_keysym_from_name("alt")   == 0x0038);
    MD_CHECK(md_input_keysym_from_name("super") == 0x007D);
    MD_CHECK(md_input_keysym_from_name("meta")  == 0x007D);
    MD_CHECK(md_input_keysym_from_name("win")   == 0x007D);

    /* Special keys */
    MD_CHECK(md_input_keysym_from_name("enter")     == 0x001C);
    MD_CHECK(md_input_keysym_from_name("return")    == 0x001C);
    MD_CHECK(md_input_keysym_from_name("tab")       == 0x000F);
    MD_CHECK(md_input_keysym_from_name("escape")    == 0x0001);
    MD_CHECK(md_input_keysym_from_name("esc")       == 0x0001);
    MD_CHECK(md_input_keysym_from_name("backspace") == 0x000E);
    MD_CHECK(md_input_keysym_from_name("delete")    == 0x006F);
    MD_CHECK(md_input_keysym_from_name("space")     == 0x0039);

    /* Navigation */
    MD_CHECK(md_input_keysym_from_name("up")       == 0x0067);
    MD_CHECK(md_input_keysym_from_name("down")     == 0x006C);
    MD_CHECK(md_input_keysym_from_name("left")     == 0x0069);
    MD_CHECK(md_input_keysym_from_name("right")    == 0x006A);
    MD_CHECK(md_input_keysym_from_name("home")     == 0x0066);
    MD_CHECK(md_input_keysym_from_name("end")      == 0x006B);
    MD_CHECK(md_input_keysym_from_name("pageup")   == 0x0068);
    MD_CHECK(md_input_keysym_from_name("pagedown") == 0x006D);

    /* F-keys */
    MD_CHECK(md_input_keysym_from_name("f1")  == 0x003B);
    MD_CHECK(md_input_keysym_from_name("f12") == 0x0058);

    /* Letters */
    MD_CHECK(md_input_keysym_from_name("a") == 0x001E);
    MD_CHECK(md_input_keysym_from_name("z") == 0x002C);
    MD_CHECK(md_input_keysym_from_name("s") == 0x001F);

    /* Digits */
    MD_CHECK(md_input_keysym_from_name("0") == 0x000B);
    MD_CHECK(md_input_keysym_from_name("1") == 0x0002);
    MD_CHECK(md_input_keysym_from_name("9") == 0x000A);

    /* Punctuation */
    MD_CHECK(md_input_keysym_from_name("-")  == 0x000C);
    MD_CHECK(md_input_keysym_from_name("=")  == 0x000D);
    MD_CHECK(md_input_keysym_from_name(",")  == 0x0033);
    MD_CHECK(md_input_keysym_from_name(".")  == 0x0034);
    MD_CHECK(md_input_keysym_from_name("/")  == 0x0035);

    /* Case insensitivity */
    MD_CHECK(md_input_keysym_from_name("CTRL")  == 0x001D);
    MD_CHECK(md_input_keysym_from_name("Shift") == 0x002A);
    MD_CHECK(md_input_keysym_from_name("F1")    == 0x003B);
    MD_CHECK(md_input_keysym_from_name("A")     == 0x001E);

    /* Unknown → 0 */
    MD_CHECK(md_input_keysym_from_name("nonexistent") == 0);
    MD_CHECK(md_input_keysym_from_name(NULL)          == 0);
    MD_CHECK(md_input_keysym_from_name("")            == 0);

    printf("OK\n");
    return 0;
}

/* ── Test: ASCII text table agrees with the key-name vocabulary ─ */

static bool ascii_is(char c, const char *name, bool shift) {
    uint32_t sym = 0;
    bool shifted = !shift;
    return md_input_ascii_key(c, &sym, &shifted) && sym != 0 &&
           sym == md_input_keysym_from_name(name) && shifted == shift;
}

static int test_ascii_key_table(void) {
    printf("  test_ascii_key_table... ");

    for (char c = 'a'; c <= 'z'; c++) {
        char name[2] = { c, 0 };
        MD_CHECK(ascii_is(c, name, false));
        MD_CHECK(ascii_is((char)(c - 'a' + 'A'), name, true));
    }
    for (char c = '0'; c <= '9'; c++) {
        char name[2] = { c, 0 };
        MD_CHECK(ascii_is(c, name, false));
    }
    /* US layout: each shifted symbol shares its base key. */
    const char *base = "1234567890-=[];'`\\,./";
    const char *upper = "!@#$%^&*()_+{}:\"~|<>?";
    for (size_t i = 0; base[i]; i++) {
        char name[2] = { base[i], 0 };
        MD_CHECK(ascii_is(base[i], name, false));
        MD_CHECK(ascii_is(upper[i], name, true));
    }
    MD_CHECK(ascii_is(' ', "space", false));
    MD_CHECK(ascii_is('\n', "enter", false));
    MD_CHECK(ascii_is('\t', "tab", false));

    /* Exactly printable ASCII + tab + newline; everything else refused. */
    int mapped = 0;
    for (int c = -128; c < 128; c++) {
        if (md_input_ascii_key((char)c, NULL, NULL)) {
            mapped++;
            MD_CHECK((c >= 0x20 && c <= 0x7E) || c == '\t' || c == '\n');
        }
    }
    MD_CHECK(mapped == 95 + 2);
    MD_CHECK(!md_input_ascii_key('\r', NULL, NULL));
    MD_CHECK(!md_input_ascii_key('\x7f', NULL, NULL));
    MD_CHECK(!md_input_ascii_key((char)0xC3, NULL, NULL)); /* UTF-8 lead byte */

    printf("OK\n");
    return 0;
}

/* ── Test: dbl_click and right_click ─────────────────────────── */

static int test_dbl_and_right_click(void) {
    printf("  test_dbl_and_right_click... ");

    MockInputData data = {0};
    MdInput inp = make_mock_input(&data, &mock_backend);

    /* Double click: two move+press+release cycles at (500,300) */
    MD_CHECK(md_input_dbl_click(&inp, 500, 300) == 0);
    MD_CHECK(data.mouse_move_calls == 2);
    MD_CHECK(data.mouse_button_calls == 4);
    MD_CHECK(data.last_x == 500 && data.last_y == 300);

    /* Right click: one cycle on the right button */
    data = (MockInputData){0};
    inp = make_mock_input(&data, &mock_backend);
    MD_CHECK(md_input_right_click(&inp, 500, 300) == 0);
    MD_CHECK(data.mouse_move_calls == 1);
    MD_CHECK(data.mouse_button_calls == 2);
    MD_CHECK(data.button_pressed[MD_MOUSE_RIGHT] == 0);

    /* NULL input */
    MD_CHECK(md_input_dbl_click(NULL, 0, 0) == -1);
    MD_CHECK(md_input_right_click(NULL, 0, 0) == -1);

    printf("OK\n");
    return 0;
}

/* ── Test: mouse_move and scroll ─────────────────────────────── */

static int test_mouse_scroll(void) {
    printf("  test_mouse_scroll... ");

    /* scroll with NULL */
    MD_CHECK(md_input_scroll(NULL, 0, 3) == -1);

    /* mouse_move with NULL */
    MD_CHECK(md_input_mouse_move(NULL, 100, 100) == -1);

    /* Deterministic mock assertions */
    MockInputData data = {0};
    MdInput inp = make_mock_input(&data, &mock_backend);

    MD_CHECK(md_input_scroll(&inp, 0, 3) == 0);
    MD_CHECK(data.mouse_scroll_calls == 1);
    MD_CHECK(data.last_dy == 3);

    MD_CHECK(md_input_mouse_move(&inp, 100, 200) == 0);
    MD_CHECK(data.mouse_move_calls == 1);
    MD_CHECK(data.last_x == 100 && data.last_y == 200);

    printf("OK\n");
    return 0;
}

/* ── Test: type_text and key_combo edge cases ────────────────── */

static int test_type_key_combo_edges(void) {
    printf("  test_type_key_combo_edges... ");

    MockInputData data = {0};
    MdInput mock = make_mock_input(&data, &mock_backend);
    MdInput *inp = &mock;

    /* NULL text */
    MD_CHECK(md_input_type_text(inp, NULL) == -1);
    MD_CHECK(md_input_type_text(NULL, "hello") == -1);

    /* NULL keys array */
    MD_CHECK(md_input_key_combo(inp, NULL, 1) == -1);

    /* Zero key count */
    const char *keys[] = {"ctrl"};
    MD_CHECK(md_input_key_combo(inp, keys, 0) == -1);

    /* NULL input */
    MD_CHECK(md_input_key_combo(NULL, keys, 1) == -1);

    /* Unknown key name should fail */
    const char *bad_keys[] = {"nonexistent_key"};
    MD_CHECK(md_input_key_combo(inp, bad_keys, 1) == -1);
    printf("OK\n");
    return 0;
}

/* ── Test: click propagates backend failures ─────────────────── */

static int test_click_backend_failure_propagation(void) {
    printf("  test_click_backend_failure_propagation... ");

    MockInputData data = {0};
    MdInput inp = make_mock_input(&data, &mock_backend);

    MD_CHECK(md_input_click(&inp, 10, 20, MD_MOUSE_LEFT) == 0);
    MD_CHECK(data.mouse_move_calls == 1);
    MD_CHECK(data.mouse_button_calls == 2);
    MD_CHECK(data.last_x == 10);
    MD_CHECK(data.last_y == 20);
    MD_CHECK(data.button_pressed[MD_MOUSE_LEFT] == 0);

    data = (MockInputData){ .mouse_move_ret = -1 };
    inp = make_mock_input(&data, &mock_backend);
    MD_CHECK(md_input_click(&inp, 10, 20, MD_MOUSE_LEFT) == -1);
    MD_CHECK(data.mouse_move_calls == 1);
    MD_CHECK(data.mouse_button_calls == 0);

    data = (MockInputData){ .mouse_button_fail_call = 1 };
    inp = make_mock_input(&data, &mock_backend);
    MD_CHECK(md_input_click(&inp, 10, 20, MD_MOUSE_LEFT) == -1);
    MD_CHECK(data.mouse_move_calls == 1);
    MD_CHECK(data.mouse_button_calls == 1);

    data = (MockInputData){ .mouse_button_fail_call = 2 };
    inp = make_mock_input(&data, &mock_backend);
    MD_CHECK(md_input_click(&inp, 10, 20, MD_MOUSE_LEFT) == -1);
    MD_CHECK(data.mouse_move_calls == 1);
    MD_CHECK(data.mouse_button_calls == 2);

    static const MdInputBackend missing_button_backend = {
        .mouse_move = mock_mouse_move,
    };
    data = (MockInputData){0};
    inp = make_mock_input(&data, &missing_button_backend);
    MD_CHECK(md_input_click(&inp, 10, 20, MD_MOUSE_LEFT) == -1);
    MD_CHECK(data.mouse_move_calls == 0);

    printf("OK\n");
    return 0;
}

/* ── Test: key_combo propagates backend failures ─────────────── */

static int test_key_combo_backend_failure_propagation(void) {
    printf("  test_key_combo_backend_failure_propagation... ");

    const char *keys[] = { "ctrl", "a" };
    MockInputData data = {0};
    MdInput inp = make_mock_input(&data, &mock_backend);

    MD_CHECK(md_input_key_combo(&inp, keys, 2) == 0);
    MD_CHECK(data.key_event_calls == 4);
    MD_CHECK(data.keysyms[0] == 0x001D && data.key_pressed[0] == 1);
    MD_CHECK(data.keysyms[1] == 0x001E && data.key_pressed[1] == 1);
    MD_CHECK(data.keysyms[2] == 0x001E && data.key_pressed[2] == 0);
    MD_CHECK(data.keysyms[3] == 0x001D && data.key_pressed[3] == 0);

    data = (MockInputData){ .key_event_fail_call = 1 };
    inp = make_mock_input(&data, &mock_backend);
    MD_CHECK(md_input_key_combo(&inp, keys, 2) == -1);
    MD_CHECK(data.key_event_calls == 1);

    data = (MockInputData){ .key_event_fail_call = 2 };
    inp = make_mock_input(&data, &mock_backend);
    MD_CHECK(md_input_key_combo(&inp, keys, 2) == -1);
    MD_CHECK(data.key_event_calls == 3); /* failed second press + ctrl cleanup */
    MD_CHECK(data.keysyms[2] == 0x001D && data.key_pressed[2] == 0);

    data = (MockInputData){ .key_event_fail_call = 3 };
    inp = make_mock_input(&data, &mock_backend);
    MD_CHECK(md_input_key_combo(&inp, keys, 2) == -1);
    MD_CHECK(data.key_event_calls == 4); /* release failure still releases rest */
    MD_CHECK(data.keysyms[3] == 0x001D && data.key_pressed[3] == 0);

    printf("OK\n");
    return 0;
}

/* ── Test: is_ready and destroy NULL ─────────────────────────── */

static int test_lifecycle_edges(void) {
    printf("  test_lifecycle_edges... ");

    MD_CHECK(md_input_is_ready(NULL) == false);
    md_input_destroy(NULL); /* should not crash */

    printf("OK\n");
    return 0;
}

static int test_explicit_backend_selection(void) {
    MdInputConfig cfg = { .screen_width = 640, .screen_height = 480,
                          .backend = "not-a-backend" };
    MD_CHECK(md_input_create(&cfg) == NULL);
    cfg.backend = "wayland";
    /* Even an enabled Wayland backend cannot use ambient display/socket FDs. */
    MD_CHECK(md_input_create(&cfg) == NULL);
    cfg.wayland_display = "wayland-0";
    MD_CHECK(md_input_create(&cfg) == NULL);
    cfg.wayland_display = "/nonexistent/metadesk-private-wayland";
    MD_CHECK(md_input_create(&cfg) == NULL);
    return 0;
}

#ifdef MD_HAVE_WAYLAND_INPUT
#include "input_wayland_server.h"
#include <pthread.h>
static void *wayland_concurrent_moves(void *data) {
    MdInput *inp = data;
    for (int i = 0; i < 16; i++) {
        MD_CHECK(md_input_is_ready(inp));
        MD_CHECK(md_input_mouse_move(inp, i, i) == 0);
    }
    return NULL;
}
static int test_wayland_protocol(void) {
    InputTestServer *s = input_test_server_start(7);
    MD_CHECK(s != NULL);
    if (!s) return 1;
    MdInputConfig cfg = { .screen_width = 640, .screen_height = 480,
        .backend = "wayland", .wayland_display = input_test_server_path(s) };
    MdInput *inp = md_input_create(&cfg);
    MD_CHECK(md_input_is_ready(inp));
    if (!inp) { input_test_server_stop(s); return 1; }
    MD_CHECK(md_input_mouse_move(inp, -10, 999) == 0);
    InputTestEvents e = input_test_server_events(s);
    MD_CHECK(e.x == 0 && e.y == 479 && e.width == 640 && e.height == 480);
    for (int b = 0; b < 3; b++) MD_CHECK(md_input_click(inp, 123, 234, b) == 0);
    MD_CHECK(inp->vtable->mouse_button(inp, 3, 1) == -1);
    MD_CHECK(md_input_scroll(inp, -2, 3) == 0);
    MD_CHECK(md_input_type_text(inp, "aA!\t\n") == 0);
    e = input_test_server_events(s);
    MD_CHECK(e.keymap_valid && e.keymaps == 1);
    MD_CHECK(e.moves == 4 && e.frames >= 11);
    MD_CHECK(e.x == 123 && e.y == 234);
    for (int b = 0; b < 3; b++) MD_CHECK(e.button_down[b] == 1 && e.button_up[b] == 1);
    MD_CHECK(e.scroll_x == -2 && e.scroll_y == -3);
    MD_CHECK(e.key_down == 5 && e.key_up == 5 && e.saw_shift);
    MD_CHECK(strcmp(e.text, "aA!\t\r") == 0);
    MD_CHECK(strcmp(e.release_text, "aA!\t\r") == 0);
    MD_CHECK(md_input_type_text(inp, "prefix\r") == -1);
    MD_CHECK(md_input_type_text(inp, "prefix\xc3\xa9") == -1);
    MD_CHECK(inp->vtable->key_event(inp, 0x1002603, 1) == -1);
    MD_CHECK(input_test_server_events(s).key_down == e.key_down);
    const char *combo[] = {"ctrl", "s"};
    MD_CHECK(md_input_key_combo(inp, combo, 2) == 0);
    MD_CHECK(input_test_server_events(s).saw_ctrl);
    pthread_t threads[2];
    unsigned started = 0;
    for (unsigned i = 0; i < 2; i++) {
        int rc = pthread_create(&threads[started], NULL, wayland_concurrent_moves, inp);
        MD_CHECK(rc == 0);
        if (!rc) started++;
    }
    for (unsigned i = 0; i < started; i++) MD_CHECK(pthread_join(threads[i], NULL) == 0);
    MD_CHECK(input_test_server_events(s).moves == 36);
    /* Deliberately leave a modifier and button down; destroy must release both. */
    MD_CHECK(inp->vtable->key_event(inp, md_input_keysym_from_name("shift"), 1) == 0);
    MD_CHECK(inp->vtable->mouse_button(inp, 0, 1) == 0);
    MD_CHECK(md_input_type_text(inp, "no") == -1);
    md_input_destroy(inp);
    e = input_test_server_events(s);
    MD_CHECK(e.key_down == e.key_up);
    MD_CHECK(e.button_down[0] == e.button_up[0]);
    /* Production constructor normalization without touching a desktop backend. */
    cfg.screen_width = 0;
    cfg.screen_height = 1;
    inp = md_input_create(&cfg);
    MD_CHECK(md_input_is_ready(inp));
    if (inp) {
        MD_CHECK(inp->config.screen_width == MD_INPUT_FALLBACK_SCREEN_WIDTH);
        MD_CHECK(inp->config.screen_height == MD_INPUT_FALLBACK_SCREEN_HEIGHT);
        md_input_destroy(inp);
    }
    input_test_server_stop(s);
    cfg.screen_width = 640;
    cfg.screen_height = 480;

    const unsigned failures[] = {0, 3, 5, 6, 7 | 8, 7 | 16, 7 | 32};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        s = input_test_server_start(failures[i]);
        MD_CHECK(s != NULL);
        if (!s) return 1;
        cfg.wayland_display = input_test_server_path(s);
        inp = md_input_create(&cfg);
        if (failures[i] & 16) {
            MD_CHECK(md_input_is_ready(inp));
            MD_CHECK(md_input_mouse_move(inp, 1, 2) == -1);
            MD_CHECK(!md_input_is_ready(inp));
            MD_CHECK(md_input_type_text(inp, "x") == -1);
        } else MD_CHECK(inp == NULL);
        md_input_destroy(inp);
        input_test_server_stop(s);
    }
    return 0;
}
#endif

/* ── Test: frame → selected-output coordinate mapping ────────── */

/* Pure math behind cg_mouse_move (no event is posted). Bounds follow the
 * CG global space: the main display at the origin, others offset from it. */
static int test_map_to_output(void) {
    printf("  test_map_to_output... ");
    double gx = -1, gy = -1;

    /* No output bounds: legacy passthrough, deliberately unclamped (MCP mode
     * feeds global accessibility coordinates against fallback dimensions). */
    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080 };
    md_input_map_to_output(&cfg, 100, 200, &gx, &gy);
    MD_CHECK(gx == 100 && gy == 200);
    md_input_map_to_output(&cfg, 2500, -5, &gx, &gy);
    MD_CHECK(gx == 2500 && gy == -5);

    /* Main display, frame == bounds: identity. */
    cfg = (MdInputConfig){ .screen_width = 1512, .screen_height = 982,
        .output_x = 0, .output_y = 0, .output_width = 1512, .output_height = 982 };
    md_input_map_to_output(&cfg, 756, 491, &gx, &gy);
    MD_CHECK(gx == 756 && gy == 491);

    /* Secondary display left of and above main (negative origin). */
    cfg = (MdInputConfig){ .screen_width = 2560, .screen_height = 1440,
        .output_x = -2560, .output_y = -300, .output_width = 2560, .output_height = 1440 };
    md_input_map_to_output(&cfg, 0, 0, &gx, &gy);
    MD_CHECK(gx == -2560 && gy == -300);
    md_input_map_to_output(&cfg, 1280, 720, &gx, &gy);
    MD_CHECK(gx == -1280 && gy == 420);

    /* Display to the right; out-of-frame points clamp to its last pixel and
     * never spill onto a neighbour. */
    cfg = (MdInputConfig){ .screen_width = 1920, .screen_height = 1080,
        .output_x = 1512, .output_y = 0, .output_width = 1920, .output_height = 1080 };
    md_input_map_to_output(&cfg, -10, 5000, &gx, &gy);
    MD_CHECK(gx == 1512 && gy == 1079);
    md_input_map_to_output(&cfg, 99999, -1, &gx, &gy);
    MD_CHECK(gx == 1512 + 1919 && gy == 0);

    /* Frame scaled relative to the display bounds (e.g. a downscaled
     * capture): coordinates scale into the bounds. */
    cfg = (MdInputConfig){ .screen_width = 960, .screen_height = 540,
        .output_x = 1512, .output_y = 100, .output_width = 1920, .output_height = 1080 };
    md_input_map_to_output(&cfg, 480, 270, &gx, &gy);
    MD_CHECK(gx == 1512 + 960 && gy == 100 + 540);
    md_input_map_to_output(&cfg, 959, 539, &gx, &gy);
    MD_CHECK(gx < 1512 + 1920 && gy < 100 + 1080);

    /* NULL arguments are ignored. */
    md_input_map_to_output(NULL, 1, 1, &gx, &gy);
    md_input_map_to_output(&cfg, 1, 1, NULL, &gy);
    printf("OK\n");
    return 0;
}

/* Agent actions carry global (accessibility) coordinates: execute_action
 * must hand the backend frame-relative points that map back to the same
 * global point, and refuse targets on other displays. */
static int test_action_global_coordinates(void) {
    printf("  test_action_global_coordinates... ");
    MockInputData data = {0};
    MdInput inp = make_mock_input(&data, &mock_backend);
    inp.config = (MdInputConfig){ .screen_width = 2560, .screen_height = 1440,
        .output_x = -2560, .output_y = -300, .output_width = 2560, .output_height = 1440 };
    MdAction action;
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_CLICK;
    action.region[0] = -1280;
    action.region[1] = 420;
    MD_CHECK(md_input_execute_action(&inp, &action) == 0);
    MD_CHECK(data.last_x == 1280 && data.last_y == 720);
    double gx, gy;
    md_input_map_to_output(&inp.config, data.last_x, data.last_y, &gx, &gy);
    MD_CHECK(gx == -1280 && gy == 420);

    /* A point on the main display is not on the captured output. */
    data = (MockInputData){0};
    action.type = MD_ACTION_DBL_CLICK;
    action.region[0] = 10;
    action.region[1] = 10;
    MD_CHECK(md_input_execute_action(&inp, &action) == -1);
    action.type = MD_ACTION_RIGHT_CLICK;
    action.region[0] = -2561;
    MD_CHECK(md_input_execute_action(&inp, &action) == -1);
    MD_CHECK(data.mouse_move_calls == 0 && data.mouse_button_calls == 0);

    /* Without output bounds, action coordinates pass through untouched. */
    inp.config = (MdInputConfig){ .screen_width = 1920, .screen_height = 1080 };
    action.type = MD_ACTION_CLICK;
    action.region[0] = 2500;
    action.region[1] = 30;
    MD_CHECK(md_input_execute_action(&inp, &action) == 0);
    MD_CHECK(data.last_x == 2500 && data.last_y == 30);
    printf("OK\n");
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_input: input HAL tests (desktop devices opt-in only)\n");

    if (access("/dev/uinput", W_OK) != 0) {
        printf("  NOTE: /dev/uinput not writable — device tests will be limited\n");
    }

    int failures = 0;
    failures += test_explicit_backend_selection();
#ifdef MD_HAVE_WAYLAND_INPUT
    failures += test_wayland_protocol();
#endif
    failures += test_create_destroy();
    failures += test_create_defaults();
    failures += test_action_dispatch();
    failures += test_action_from_json();
    failures += test_keysym_lookup();
    failures += test_ascii_key_table();
    failures += test_dbl_and_right_click();
    failures += test_mouse_scroll();
    failures += test_type_key_combo_edges();
    failures += test_click_backend_failure_propagation();
    failures += test_key_combo_backend_failure_propagation();
    failures += test_lifecycle_edges();
    failures += test_map_to_output();
    failures += test_action_global_coordinates();

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    md_test_failures += failures;
    return md_test_report();
}
