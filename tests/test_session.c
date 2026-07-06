/*
 * metadesk — test_session.c
 * Tests for session JSON serialization and state machine.
 */
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Session request round-trip ──────────────────────────────── */

static void test_request_roundtrip(void) {
    MdSessionRequest req = {
        .capabilities = MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT,
        .tree_format  = MD_TREE_FORMAT_COMPACT,
    };
    strncpy(req.fips_addr, "npub1testaddr", sizeof(req.fips_addr) - 1);

    char *json = md_session_request_to_json(&req);
    assert(json != NULL);
    assert(strlen(json) > 0);

    /* Verify expected fields are present */
    assert(strstr(json, "\"session_request\"") != NULL);
    assert(strstr(json, "\"v\":1") != NULL);
    assert(strstr(json, "\"video\"") != NULL);
    assert(strstr(json, "\"agent\"") != NULL);
    assert(strstr(json, "\"input\"") != NULL);
    assert(strstr(json, "\"compact\"") != NULL);
    assert(strstr(json, "npub1testaddr") != NULL);

    /* Parse back and verify all fields */
    MdSessionRequest out;
    int ret = md_session_request_from_json(json, &out);
    assert(ret == 0);
    assert(out.capabilities == (MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT));
    assert(out.tree_format == MD_TREE_FORMAT_COMPACT);
    assert(strcmp(out.fips_addr, "npub1testaddr") == 0);

    free(json);
    printf("  PASS: request round-trip\n");
}

/* ── Session accept round-trip ───────────────────────────────── */

static void test_accept_roundtrip(void) {
    MdSessionAccept acc = {
        .granted = MD_CAP_VIDEO | MD_CAP_AGENT,
        .tree_format = MD_TREE_FORMAT_COMPACT,
    };
    strncpy(acc.session_id, "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
            sizeof(acc.session_id) - 1);

    char *json = md_session_accept_to_json(&acc);
    assert(json != NULL);
    assert(strlen(json) > 0);

    /* Verify expected fields */
    assert(strstr(json, "\"session_accept\"") != NULL);
    assert(strstr(json, "\"v\":1") != NULL);
    assert(strstr(json, "a1b2c3d4-e5f6-7890-abcd-ef1234567890") != NULL);
    assert(strstr(json, "\"video\"") != NULL);
    assert(strstr(json, "\"agent\"") != NULL);
    /* input was NOT granted */
    assert(strstr(json, "\"input\"") == NULL);
    /* tree_format confirmed as compact */
    assert(strstr(json, "\"compact\"") != NULL);

    /* Parse back and verify */
    MdSessionAccept out;
    int ret = md_session_accept_from_json(json, &out);
    assert(ret == 0);
    assert(out.granted == (MD_CAP_VIDEO | MD_CAP_AGENT));
    assert(strcmp(out.session_id, "a1b2c3d4-e5f6-7890-abcd-ef1234567890") == 0);
    assert(out.tree_format == MD_TREE_FORMAT_COMPACT);

    free(json);
    printf("  PASS: accept round-trip\n");
}

/* ── Partial capabilities ────────────────────────────────────── */

static void test_single_capability(void) {
    MdSessionRequest req = { .capabilities = MD_CAP_VIDEO };

    char *json = md_session_request_to_json(&req);
    assert(json != NULL);
    assert(strstr(json, "\"video\"") != NULL);
    assert(strstr(json, "\"agent\"") == NULL);
    assert(strstr(json, "\"input\"") == NULL);

    MdSessionRequest out;
    assert(md_session_request_from_json(json, &out) == 0);
    assert(out.capabilities == MD_CAP_VIDEO);

    free(json);
    printf("  PASS: single capability\n");
}

static void test_no_capabilities(void) {
    MdSessionRequest req = { .capabilities = 0 };

    char *json = md_session_request_to_json(&req);
    assert(json != NULL);

    MdSessionRequest out;
    assert(md_session_request_from_json(json, &out) == 0);
    assert(out.capabilities == 0);

    free(json);
    printf("  PASS: no capabilities\n");
}

/* ── Tree format variations ──────────────────────────────────── */

static void test_tree_format_json(void) {
    MdSessionRequest req = {
        .capabilities = MD_CAP_VIDEO,
        .tree_format  = MD_TREE_FORMAT_JSON,
    };

    char *json = md_session_request_to_json(&req);
    assert(json != NULL);
    assert(strstr(json, "\"json\"") != NULL);

    MdSessionRequest out;
    assert(md_session_request_from_json(json, &out) == 0);
    assert(out.tree_format == MD_TREE_FORMAT_JSON);

    free(json);
    printf("  PASS: tree format json\n");
}

/* ── Empty fips_addr ─────────────────────────────────────────── */

static void test_empty_fips_addr(void) {
    MdSessionRequest req = {
        .capabilities = MD_CAP_VIDEO,
        .tree_format  = MD_TREE_FORMAT_COMPACT,
    };
    /* fips_addr left empty (zeroed) */

    char *json = md_session_request_to_json(&req);
    assert(json != NULL);

    MdSessionRequest out;
    assert(md_session_request_from_json(json, &out) == 0);
    assert(out.fips_addr[0] == '\0');

    free(json);
    printf("  PASS: empty fips_addr\n");
}

/* ── Empty session_id in accept ──────────────────────────────── */

static void test_empty_session_id(void) {
    MdSessionAccept acc = { .granted = MD_CAP_VIDEO };
    /* session_id left empty */

    char *json = md_session_accept_to_json(&acc);
    assert(json != NULL);

    MdSessionAccept out;
    assert(md_session_accept_from_json(json, &out) == 0);
    assert(out.session_id[0] == '\0');
    assert(out.granted == MD_CAP_VIDEO);

    free(json);
    printf("  PASS: empty session_id\n");
}

/* ── Wrong type field rejected ───────────────────────────────── */

static void test_wrong_type_rejected(void) {
    /* session_accept JSON should not parse as session_request */
    MdSessionAccept acc = { .granted = MD_CAP_VIDEO };
    strncpy(acc.session_id, "test-id", sizeof(acc.session_id) - 1);

    char *json = md_session_accept_to_json(&acc);
    assert(json != NULL);

    MdSessionRequest req_out;
    int ret = md_session_request_from_json(json, &req_out);
    assert(ret == -1); /* wrong type → parse failure */

    free(json);

    /* And vice versa */
    MdSessionRequest req = { .capabilities = MD_CAP_VIDEO };
    json = md_session_request_to_json(&req);
    assert(json != NULL);

    MdSessionAccept acc_out;
    ret = md_session_accept_from_json(json, &acc_out);
    assert(ret == -1);

    free(json);
    printf("  PASS: wrong type rejected\n");
}

/* ── NULL and invalid input ──────────────────────────────────── */

static void test_null_args(void) {
    /* NULL request/accept → NULL json */
    assert(md_session_request_to_json(NULL) == NULL);
    assert(md_session_accept_to_json(NULL) == NULL);

    /* NULL json → error */
    MdSessionRequest req;
    assert(md_session_request_from_json(NULL, &req) == -1);
    assert(md_session_request_from_json("{}", NULL) == -1);

    MdSessionAccept acc;
    assert(md_session_accept_from_json(NULL, &acc) == -1);
    assert(md_session_accept_from_json("{}", NULL) == -1);

    /* Invalid JSON → error */
    assert(md_session_request_from_json("not json", &req) == -1);
    assert(md_session_accept_from_json("{broken", &acc) == -1);

    /* Valid JSON but missing type → error */
    assert(md_session_request_from_json("{\"v\":1}", &req) == -1);
    assert(md_session_accept_from_json("{\"v\":1}", &acc) == -1);

    printf("  PASS: null/invalid args\n");
}

/* ── Capability helpers ──────────────────────────────────────── */

static void test_caps_helpers(void) {
    const char *strs[] = {"video", "agent", "input"};
    uint32_t caps = md_caps_from_strings(strs, 3);
    assert(caps == (MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT));

    /* Unknown capability name is ignored */
    const char *strs2[] = {"video", "unknown_cap", "input"};
    caps = md_caps_from_strings(strs2, 3);
    assert(caps == (MD_CAP_VIDEO | MD_CAP_INPUT));

    /* Empty array */
    caps = md_caps_from_strings(NULL, 0);
    assert(caps == 0);

    /* Round-trip through to_strings */
    caps = MD_CAP_VIDEO | MD_CAP_INPUT;
    const char *out[8];
    int n = md_caps_to_strings(caps, out, 8);
    assert(n == 2);
    /* Verify both are present (order depends on cap_table) */
    bool found_video = false, found_input = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i], "video") == 0) found_video = true;
        if (strcmp(out[i], "input") == 0) found_input = true;
    }
    assert(found_video);
    assert(found_input);

    printf("  PASS: capability helpers\n");
}

/* ── Session state machine ───────────────────────────────────── */

static void test_state_machine(void) {
    MdSession s;
    md_session_init(&s);
    assert(s.state == MD_SESSION_IDLE);
    assert(s.keepalive_ms == MD_SESSION_KEEPALIVE_DEFAULT_MS);

    /* Host side can accept an incoming session from IDLE */
    assert(md_session_accept(&s, "host-side-id", MD_CAP_VIDEO) == 0);
    assert(s.state == MD_SESSION_NEGOTIATING);
    assert(strcmp(s.session_id, "host-side-id") == 0);

    /* Reset returns DISCONNECTING/NEGOTIATING sessions to IDLE */
    md_session_reset(&s);
    assert(s.state == MD_SESSION_IDLE);
    assert(s.keepalive_ms == MD_SESSION_KEEPALIVE_DEFAULT_MS);

    /* Can't activate from IDLE */
    assert(md_session_activate(&s) == -1);

    /* Request transitions to REQUESTING */
    assert(md_session_request(&s, "npub1abc", MD_CAP_VIDEO | MD_CAP_AGENT,
                              MD_TREE_FORMAT_COMPACT) == 0);
    assert(s.state == MD_SESSION_REQUESTING);
    assert(s.capabilities == (MD_CAP_VIDEO | MD_CAP_AGENT));
    assert(s.tree_format == MD_TREE_FORMAT_COMPACT);
    assert(strcmp(s.peer_npub, "npub1abc") == 0);

    /* Can't request again from REQUESTING */
    assert(md_session_request(&s, "npub1xyz", MD_CAP_VIDEO, MD_TREE_FORMAT_JSON) == -1);

    /* Accept transitions to NEGOTIATING */
    assert(md_session_accept(&s, "session-uuid-1", MD_CAP_VIDEO) == 0);
    assert(s.state == MD_SESSION_NEGOTIATING);
    assert(s.capabilities == MD_CAP_VIDEO); /* granted overrides requested */
    assert(strcmp(s.session_id, "session-uuid-1") == 0);

    /* Activate transitions to ACTIVE */
    assert(md_session_activate(&s) == 0);
    assert(s.state == MD_SESSION_ACTIVE);

    /* Disconnect from ACTIVE */
    assert(md_session_disconnect(&s) == 0);
    assert(s.state == MD_SESSION_DISCONNECTING);

    /* Reset is the path from DISCONNECTING back to IDLE */
    md_session_reset(&s);
    assert(s.state == MD_SESSION_IDLE);
    assert(s.session_id[0] == '\0');

    /* Can't disconnect from IDLE */
    md_session_init(&s);
    assert(md_session_disconnect(&s) == -1);

    printf("  PASS: state machine\n");
}

static void test_accept_from_idle_host_side(void) {
    MdSession s;
    md_session_init(&s);

    assert(md_session_accept(&s, "host-side-session", MD_CAP_VIDEO | MD_CAP_AGENT) == 0);
    assert(s.state == MD_SESSION_NEGOTIATING);
    assert(s.capabilities == (MD_CAP_VIDEO | MD_CAP_AGENT));
    assert(strcmp(s.session_id, "host-side-session") == 0);

    printf("  PASS: accept from IDLE host side\n");
}

static void test_reset_after_disconnect(void) {
    MdSession s;
    md_session_init(&s);

    assert(md_session_request(&s, "npub1peer", MD_CAP_VIDEO,
                              MD_TREE_FORMAT_COMPACT) == 0);
    assert(md_session_accept(&s, "disconnect-reset-session", MD_CAP_VIDEO) == 0);
    assert(md_session_activate(&s) == 0);
    assert(md_session_disconnect(&s) == 0);
    assert(s.state == MD_SESSION_DISCONNECTING);

    md_session_reset(&s);
    assert(s.state == MD_SESSION_IDLE);
    assert(s.capabilities == 0);
    assert(s.tree_format == MD_TREE_FORMAT_JSON);
    assert(s.session_id[0] == '\0');
    assert(s.peer_npub[0] == '\0');
    assert(s.keepalive_ms == MD_SESSION_KEEPALIVE_DEFAULT_MS);

    printf("  PASS: reset after disconnect\n");
}

static void test_invalid_capability_strings_rejected(void) {
    MdSessionRequest req;
    const char *bad_req = "{\"type\":\"session_request\",\"v\":1,"
                          "\"capabilities\":[\"video\",\"clipboard\"],"
                          "\"tree_format\":\"json\"}";
    assert(md_session_request_from_json(bad_req, &req) == -1);

    const char *bad_req_type = "{\"type\":\"session_request\",\"v\":1,"
                               "\"capabilities\":[\"video\",42],"
                               "\"tree_format\":\"json\"}";
    assert(md_session_request_from_json(bad_req_type, &req) == -1);

    MdSessionAccept acc;
    const char *bad_acc = "{\"type\":\"session_accept\",\"v\":1,"
                          "\"session_id\":\"sid\","
                          "\"granted\":[\"agent\",\"clipboard\"],"
                          "\"tree_format\":\"compact\"}";
    assert(md_session_accept_from_json(bad_acc, &acc) == -1);

    printf("  PASS: invalid capability strings rejected\n");
}

/* ── Keepalive timeout ───────────────────────────────────────── */

static void test_keepalive_timeout(void) {
    MdSession s;
    md_session_init(&s);
    md_session_request(&s, "npub1test", MD_CAP_VIDEO, MD_TREE_FORMAT_JSON);
    md_session_accept(&s, "sid", MD_CAP_VIDEO);
    md_session_activate(&s);

    /* Not timed out when no pongs yet */
    assert(!md_session_is_timed_out(&s, 100000));

    /* Record a pong */
    md_session_on_pong(&s, 1000);
    assert(s.last_pong_ms == 1000);

    /* Not timed out shortly after pong */
    assert(!md_session_is_timed_out(&s, 1000 + s.keepalive_ms));

    /* Clock rollback must not underflow into a false timeout */
    assert(!md_session_is_timed_out(&s, 999));

    /* Timed out after keepalive * timeout_mult */
    uint64_t timeout = (uint64_t)s.keepalive_ms * MD_SESSION_KEEPALIVE_TIMEOUT_MULT;
    assert(!md_session_is_timed_out(&s, 1000 + timeout));
    assert(md_session_is_timed_out(&s, 1000 + timeout + 1));

    /* Ping returns true when ACTIVE */
    assert(md_session_on_ping(&s, 2000) == true);
    assert(s.last_ping_ms == 2000);

    printf("  PASS: keepalive timeout\n");
}

/* ── NULL session handling ───────────────────────────────────── */

static void test_null_session(void) {
    md_session_init(NULL);
    assert(md_session_request(NULL, "npub", 0, MD_TREE_FORMAT_JSON) == -1);
    assert(md_session_accept(NULL, "id", 0) == -1);
    assert(md_session_activate(NULL) == -1);
    assert(md_session_disconnect(NULL) == -1);
    md_session_reset(NULL);
    assert(md_session_on_ping(NULL, 0) == false);
    md_session_on_pong(NULL, 0);
    assert(md_session_is_timed_out(NULL, 0) == false);

    printf("  PASS: null session handling\n");
}

/* ── Tree format negotiation in accept ────────────────────────── */

static void test_accept_tree_format_negotiation(void) {
    /* Compact format round-trips through accept */
    MdSessionAccept acc = {
        .granted = MD_CAP_VIDEO,
        .tree_format = MD_TREE_FORMAT_COMPACT,
    };
    strncpy(acc.session_id, "fmt-test-1", sizeof(acc.session_id) - 1);

    char *json = md_session_accept_to_json(&acc);
    assert(json != NULL);
    assert(strstr(json, "\"compact\"") != NULL);

    MdSessionAccept out;
    assert(md_session_accept_from_json(json, &out) == 0);
    assert(out.tree_format == MD_TREE_FORMAT_COMPACT);
    free(json);

    /* JSON format round-trips through accept */
    acc.tree_format = MD_TREE_FORMAT_JSON;
    json = md_session_accept_to_json(&acc);
    assert(json != NULL);
    assert(strstr(json, "\"json\"") != NULL);

    assert(md_session_accept_from_json(json, &out) == 0);
    assert(out.tree_format == MD_TREE_FORMAT_JSON);
    free(json);

    /* Missing tree_format in accept defaults to JSON (backwards compat) */
    const char *no_tf = "{\"type\":\"session_accept\",\"v\":1,"
                        "\"session_id\":\"test\",\"granted\":[\"video\"]}";
    assert(md_session_accept_from_json(no_tf, &out) == 0);
    assert(out.tree_format == MD_TREE_FORMAT_JSON);

    /* Full negotiation flow: request compact → accept confirms compact */
    MdSessionRequest req = {
        .capabilities = MD_CAP_VIDEO | MD_CAP_AGENT,
        .tree_format = MD_TREE_FORMAT_COMPACT,
    };
    char *req_json = md_session_request_to_json(&req);
    assert(req_json != NULL);

    MdSessionRequest parsed_req;
    assert(md_session_request_from_json(req_json, &parsed_req) == 0);
    assert(parsed_req.tree_format == MD_TREE_FORMAT_COMPACT);
    free(req_json);

    /* Host echoes back the format in accept */
    MdSessionAccept host_acc = {
        .granted = MD_CAP_VIDEO,
        .tree_format = parsed_req.tree_format,  /* honour client's preference */
    };
    strncpy(host_acc.session_id, "negotiated-1", sizeof(host_acc.session_id) - 1);

    char *acc_json = md_session_accept_to_json(&host_acc);
    assert(acc_json != NULL);

    MdSessionAccept client_acc;
    assert(md_session_accept_from_json(acc_json, &client_acc) == 0);
    assert(client_acc.tree_format == MD_TREE_FORMAT_COMPACT);
    free(acc_json);

    printf("  PASS: accept tree format negotiation\n");
}

int main(void) {
    printf("test_session (JSON + state machine):\n");

    /* JSON serialization tests */
    test_request_roundtrip();
    test_accept_roundtrip();
    test_single_capability();
    test_no_capabilities();
    test_tree_format_json();
    test_empty_fips_addr();
    test_empty_session_id();
    test_wrong_type_rejected();
    test_null_args();
    test_caps_helpers();

    /* State machine tests */
    test_state_machine();
    test_accept_from_idle_host_side();
    test_reset_after_disconnect();
    test_invalid_capability_strings_rejected();
    test_keepalive_timeout();
    test_null_session();
    test_accept_tree_format_negotiation();

    printf("All session tests passed.\n");
    return 0;
}
