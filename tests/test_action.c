/*
 * metadesk — tests/test_action.c
 * Unit tests for action JSON parse/encode and type mapping.
 */
#include "action.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md_test.h"


/* ── Type string mapping ─────────────────────────────────────── */

static void test_type_from_str(void)
{
    MD_CHECK(md_action_type_from_str("click")       == MD_ACTION_CLICK);
    MD_CHECK(md_action_type_from_str("dbl_click")   == MD_ACTION_DBL_CLICK);
    MD_CHECK(md_action_type_from_str("right_click") == MD_ACTION_RIGHT_CLICK);
    MD_CHECK(md_action_type_from_str("type")        == MD_ACTION_TYPE);
    MD_CHECK(md_action_type_from_str("key_combo")   == MD_ACTION_KEY_COMBO);
    MD_CHECK(md_action_type_from_str("scroll")      == MD_ACTION_SCROLL);
    MD_CHECK(md_action_type_from_str("focus")       == MD_ACTION_FOCUS);
    MD_CHECK(md_action_type_from_str("set_value")   == MD_ACTION_SET_VALUE);
    MD_CHECK(md_action_type_from_str("screenshot")  == MD_ACTION_SCREENSHOT);
    MD_CHECK(md_action_type_from_str("bogus")       == MD_ACTION_UNKNOWN);
    MD_CHECK(md_action_type_from_str(NULL)          == MD_ACTION_UNKNOWN);
    MD_CHECK(md_action_type_from_str("")            == MD_ACTION_UNKNOWN);

    PASS("type_from_str");
}

static void test_type_str(void)
{
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_CLICK),       "click")       == 0);
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_DBL_CLICK),   "dbl_click")   == 0);
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_RIGHT_CLICK), "right_click") == 0);
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_TYPE),        "type")        == 0);
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_KEY_COMBO),   "key_combo")   == 0);
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_SCROLL),      "scroll")      == 0);
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_FOCUS),       "focus")       == 0);
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_SET_VALUE),   "set_value")   == 0);
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_SCREENSHOT),  "screenshot")  == 0);
    MD_CHECK(strcmp(md_action_type_str(MD_ACTION_UNKNOWN),     "unknown")     == 0);

    PASS("type_str");
}

/* ── Parse: click action ─────────────────────────────────────── */

static void test_parse_click(void)
{
    const char *json = "{\"v\":1,\"action\":\"click\",\"target_id\":\"btn_ok\","
                       "\"payload\":{}}";
    MdAction action;
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == 0);
    MD_CHECK(action.type == MD_ACTION_CLICK);
    MD_CHECK(strcmp(action.target_id, "btn_ok") == 0);
    MD_CHECK(action.key_count == 0);
    md_action_cleanup(&action);

    PASS("parse click");
}

/* ── Parse: type action ──────────────────────────────────────── */

static void test_parse_type(void)
{
    const char *json = "{\"v\":1,\"action\":\"type\",\"target_id\":\"input_1\","
                       "\"payload\":{\"text\":\"Hello World\"}}";
    MdAction action;
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == 0);
    MD_CHECK(action.type == MD_ACTION_TYPE);
    MD_CHECK(strcmp(action.target_id, "input_1") == 0);
    MD_CHECK(strcmp(action.text, "Hello World") == 0);
    md_action_cleanup(&action);

    PASS("parse type");
}

/* ── Parse: key_combo action ─────────────────────────────────── */

static void test_parse_key_combo(void)
{
    const char *json = "{\"v\":1,\"action\":\"key_combo\","
                       "\"payload\":{\"keys\":[\"ctrl\",\"shift\",\"s\"]}}";
    MdAction action;
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == 0);
    MD_CHECK(action.type == MD_ACTION_KEY_COMBO);
    MD_CHECK(action.target_id[0] == '\0'); /* no target for key_combo */
    MD_CHECK(action.key_count == 3);
    MD_CHECK(strcmp(action.keys[0], "ctrl")  == 0);
    MD_CHECK(strcmp(action.keys[1], "shift") == 0);
    MD_CHECK(strcmp(action.keys[2], "s")     == 0);
    md_action_cleanup(&action);

    PASS("parse key_combo");
}

/* ── Parse: scroll action ────────────────────────────────────── */

static void test_parse_scroll(void)
{
    const char *json = "{\"v\":1,\"action\":\"scroll\",\"target_id\":\"list_1\","
                       "\"payload\":{\"dx\":0,\"dy\":-120}}";
    MdAction action;
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == 0);
    MD_CHECK(action.type == MD_ACTION_SCROLL);
    MD_CHECK(strcmp(action.target_id, "list_1") == 0);
    MD_CHECK(action.dx == 0);
    MD_CHECK(action.dy == -120);
    md_action_cleanup(&action);

    PASS("parse scroll");
}

/* ── Parse: screenshot action ────────────────────────────────── */

static void test_parse_screenshot(void)
{
    const char *json = "{\"v\":1,\"action\":\"screenshot\",\"target_id\":\"win_1\","
                       "\"payload\":{\"region\":[10,20,640,480]}}";
    MdAction action;
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == 0);
    MD_CHECK(action.type == MD_ACTION_SCREENSHOT);
    MD_CHECK(action.region[0] == 10);
    MD_CHECK(action.region[1] == 20);
    MD_CHECK(action.region[2] == 640);
    MD_CHECK(action.region[3] == 480);
    md_action_cleanup(&action);

    PASS("parse screenshot");
}

/* ── Parse: set_value action ─────────────────────────────────── */

static void test_parse_set_value(void)
{
    const char *json = "{\"v\":1,\"action\":\"set_value\",\"target_id\":\"slider_1\","
                       "\"payload\":{\"text\":\"75\"}}";
    MdAction action;
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == 0);
    MD_CHECK(action.type == MD_ACTION_SET_VALUE);
    MD_CHECK(strcmp(action.target_id, "slider_1") == 0);
    MD_CHECK(strcmp(action.text, "75") == 0);
    md_action_cleanup(&action);

    PASS("parse set_value");
}

/* ── Parse: focus action ─────────────────────────────────────── */

static void test_parse_focus(void)
{
    const char *json = "{\"v\":1,\"action\":\"focus\",\"target_id\":\"input_2\","
                       "\"payload\":{}}";
    MdAction action;
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == 0);
    MD_CHECK(action.type == MD_ACTION_FOCUS);
    MD_CHECK(strcmp(action.target_id, "input_2") == 0);
    md_action_cleanup(&action);

    PASS("parse focus");
}

/* ── Parse: error handling ───────────────────────────────────── */

static void test_parse_errors(void)
{
    MdAction action;

    /* NULL args */
    MD_CHECK(md_action_parse(NULL, "{}", 2) == -1);
    MD_CHECK(md_action_parse(&action, NULL, 0) == -1);
    MD_CHECK(md_action_parse(&action, "{}", 0) == -1);

    /* Malformed JSON */
    MD_CHECK(md_action_parse(&action, "{{bad", 5) == -1);

    /* Missing version */
    MD_CHECK(md_action_parse(&action, "{\"action\":\"click\"}", 18) == -1);

    /* Wrong version */
    MD_CHECK(md_action_parse(&action, "{\"v\":2,\"action\":\"click\"}", 23) == -1);

    /* Missing action field */
    MD_CHECK(md_action_parse(&action, "{\"v\":1}", 7) == -1);

    /* Non-string action */
    MD_CHECK(md_action_parse(&action, "{\"v\":1,\"action\":42}", 19) == -1);

    PASS("parse errors");
}

/* ── Parse: unknown action type is rejected ───────────── */

static void test_parse_unknown_type(void)
{
    const char *json = "{\"v\":1,\"action\":\"weird_new_action\","
                       "\"target_id\":\"x\",\"payload\":{}}";
    MdAction action;
    /* An unknown verb must be rejected at the parse boundary, not accepted
     * as a valid-looking MD_ACTION_UNKNOWN. */
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == -1);

    PASS("parse unknown type rejected");
}

/* ── Parse: max keys clamped ─────────────────────────────────── */

static void test_parse_max_keys(void)
{
    /* Build a key_combo with more than MD_MAX_KEYS (8) keys */
    const char *json = "{\"v\":1,\"action\":\"key_combo\","
                       "\"payload\":{\"keys\":"
                       "[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\","
                       "\"i\",\"j\",\"k\"]}}";
    MdAction action;
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == 0);
    MD_CHECK(action.key_count == MD_MAX_KEYS); /* clamped to 8 */
    md_action_cleanup(&action);

    PASS("parse max keys clamped");
}

/* ── Encode: click ───────────────────────────────────────────── */

static void test_encode_click(void)
{
    MdAction action;
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_CLICK;
    strncpy(action.target_id, "btn_save", sizeof(action.target_id));

    char *json = md_action_encode(&action);
    MD_CHECK(json != NULL);
    MD_CHECK(strstr(json, "\"action\":\"click\"") != NULL);
    MD_CHECK(strstr(json, "\"target_id\":\"btn_save\"") != NULL);
    MD_CHECK(strstr(json, "\"v\":1") != NULL);
    free(json);

    PASS("encode click");
}

/* ── Encode: key_combo ───────────────────────────────────────── */

static void test_encode_key_combo(void)
{
    MdAction action;
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_KEY_COMBO;
    action.keys[0] = strdup("ctrl");
    action.keys[1] = strdup("c");
    action.key_count = 2;

    char *json = md_action_encode(&action);
    MD_CHECK(json != NULL);
    MD_CHECK(strstr(json, "\"action\":\"key_combo\"") != NULL);
    MD_CHECK(strstr(json, "\"keys\":[\"ctrl\",\"c\"]") != NULL);
    free(json);

    md_action_cleanup(&action);
    PASS("encode key_combo");
}

/* ── Encode: scroll ──────────────────────────────────────────── */

static void test_encode_scroll(void)
{
    MdAction action;
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_SCROLL;
    strncpy(action.target_id, "panel", sizeof(action.target_id));
    action.dx = 10;
    action.dy = -50;

    char *json = md_action_encode(&action);
    MD_CHECK(json != NULL);
    MD_CHECK(strstr(json, "\"action\":\"scroll\"") != NULL);
    MD_CHECK(strstr(json, "\"dx\":10") != NULL);
    MD_CHECK(strstr(json, "\"dy\":-50") != NULL);
    free(json);

    PASS("encode scroll");
}

/* ── Encode: screenshot with region ──────────────────────────── */

static void test_encode_screenshot(void)
{
    MdAction action;
    memset(&action, 0, sizeof(action));
    action.type = MD_ACTION_SCREENSHOT;
    strncpy(action.target_id, "win", sizeof(action.target_id));
    action.region[0] = 0;
    action.region[1] = 0;
    action.region[2] = 1920;
    action.region[3] = 1080;

    char *json = md_action_encode(&action);
    MD_CHECK(json != NULL);
    MD_CHECK(strstr(json, "\"action\":\"screenshot\"") != NULL);
    MD_CHECK(strstr(json, "\"region\":[0,0,1920,1080]") != NULL);
    free(json);

    PASS("encode screenshot");
}

/* ── Encode: NULL action ─────────────────────────────────────── */

static void test_encode_null(void)
{
    MD_CHECK(md_action_encode(NULL) == NULL);
    PASS("encode null");
}

/* ── Round-trip: parse → encode → parse ──────────────────────── */

static void test_roundtrip(void)
{
    const char *original = "{\"v\":1,\"action\":\"type\","
                           "\"target_id\":\"field_name\","
                           "\"payload\":{\"text\":\"test input\"}}";

    /* Parse */
    MdAction a1;
    MD_CHECK(md_action_parse(&a1, original, strlen(original)) == 0);

    /* Encode */
    char *encoded = md_action_encode(&a1);
    MD_CHECK(encoded != NULL);

    /* Parse again */
    MdAction a2;
    MD_CHECK(md_action_parse(&a2, encoded, strlen(encoded)) == 0);

    /* Verify fields match */
    MD_CHECK(a1.type == a2.type);
    MD_CHECK(strcmp(a1.target_id, a2.target_id) == 0);
    MD_CHECK(strcmp(a1.text, a2.text) == 0);

    free(encoded);
    md_action_cleanup(&a1);
    md_action_cleanup(&a2);

    PASS("roundtrip parse→encode→parse");
}

/* ── Cleanup: double cleanup is safe ─────────────────────────── */

static void test_cleanup_safety(void)
{
    MdAction action;
    memset(&action, 0, sizeof(action));
    action.keys[0] = strdup("ctrl");
    action.keys[1] = strdup("v");
    action.key_count = 2;

    md_action_cleanup(&action);
    MD_CHECK(action.key_count == 0);
    MD_CHECK(action.keys[0] == NULL);
    MD_CHECK(action.keys[1] == NULL);

    /* Second cleanup should be safe */
    md_action_cleanup(&action);
    MD_CHECK(action.key_count == 0);

    /* NULL cleanup should be safe */
    md_action_cleanup(NULL);

    PASS("cleanup safety");
}

/* ── Parse: no payload object ────────────────────────────────── */

static void test_parse_no_payload(void)
{
    const char *json = "{\"v\":1,\"action\":\"click\",\"target_id\":\"x\"}";
    MdAction action;
    MD_CHECK(md_action_parse(&action, json, strlen(json)) == 0);
    MD_CHECK(action.type == MD_ACTION_CLICK);
    MD_CHECK(strcmp(action.target_id, "x") == 0);
    MD_CHECK(action.text[0] == '\0');
    MD_CHECK(action.key_count == 0);
    md_action_cleanup(&action);

    PASS("parse no payload");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_action: action parse/encode unit tests\n");

    /* Type mapping */
    test_type_from_str();
    test_type_str();

    /* Parse */
    test_parse_click();
    test_parse_type();
    test_parse_key_combo();
    test_parse_scroll();
    test_parse_screenshot();
    test_parse_set_value();
    test_parse_focus();
    test_parse_unknown_type();
    test_parse_max_keys();
    test_parse_no_payload();
    test_parse_errors();

    /* Encode */
    test_encode_click();
    test_encode_key_combo();
    test_encode_scroll();
    test_encode_screenshot();
    test_encode_null();

    /* Round-trip */
    test_roundtrip();

    /* Cleanup */
    test_cleanup_safety();

    printf("\nAll action tests passed.\n");
    return md_test_report();
}
