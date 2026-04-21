/*
 * test_fipsnat_ipc.c — Unit tests for fips-nat IPC protocol serialization.
 *
 * Tests command building, command parsing, response building, and
 * response parsing. Pure serialization — no network or IPC needed.
 */
#include "fipsnat_ipc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do {                                       \
    tests_run++;                                                \
    printf("  [%2d] %-50s ", tests_run, #fn);                   \
    fn();                                                       \
    tests_passed++;                                             \
    printf("PASS\n");                                           \
} while (0)

/* ── Command building tests ──────────────────────────────────── */

static void test_cmd_status(void) {
    char *json = md_fipsnat_ipc_cmd_status();
    assert(json);
    assert(strstr(json, "\"cmd\":\"status\""));
    free(json);
}

static void test_cmd_discover(void) {
    char *json = md_fipsnat_ipc_cmd_discover();
    assert(json);
    assert(strstr(json, "\"cmd\":\"discover\""));
    free(json);
}

static void test_cmd_shutdown(void) {
    char *json = md_fipsnat_ipc_cmd_shutdown();
    assert(json);
    assert(strstr(json, "\"cmd\":\"shutdown\""));
    free(json);
}

static void test_cmd_punch(void) {
    uint8_t sid[16] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                       0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10};
    char *json = md_fipsnat_ipc_cmd_punch("203.0.113.42", 5678, sid, 15000);
    assert(json);
    assert(strstr(json, "\"cmd\":\"punch\""));
    assert(strstr(json, "\"peer_ip\":\"203.0.113.42\""));
    assert(strstr(json, "\"peer_port\":5678"));
    assert(strstr(json, "\"session_id\":\"0123456789abcdeffedcba9876543210\""));
    assert(strstr(json, "\"timeout_ms\":15000"));
    free(json);
}

static void test_cmd_punch_no_timeout(void) {
    uint8_t sid[16] = {0};
    char *json = md_fipsnat_ipc_cmd_punch("10.0.0.1", 1234, sid, 0);
    assert(json);
    /* timeout_ms should be omitted when 0 */
    assert(!strstr(json, "timeout_ms"));
    free(json);
}

static void test_cmd_punch_null_safety(void) {
    uint8_t sid[16] = {0};
    assert(md_fipsnat_ipc_cmd_punch(NULL, 1234, sid, 0) == NULL);
    assert(md_fipsnat_ipc_cmd_punch("1.2.3.4", 1234, NULL, 0) == NULL);
}

/* ── Command parsing tests ────────���──────────────────────────── */

static void test_parse_status(void) {
    MdFipsnatCmd cmd;
    int rc = md_fipsnat_ipc_parse_command("{\"cmd\":\"status\"}", &cmd);
    assert(rc == 0);
    assert(cmd.type == MD_FIPSNAT_CMD_STATUS);
}

static void test_parse_discover(void) {
    MdFipsnatCmd cmd;
    int rc = md_fipsnat_ipc_parse_command("{\"cmd\":\"discover\"}", &cmd);
    assert(rc == 0);
    assert(cmd.type == MD_FIPSNAT_CMD_DISCOVER);
}

static void test_parse_shutdown(void) {
    MdFipsnatCmd cmd;
    int rc = md_fipsnat_ipc_parse_command("{\"cmd\":\"shutdown\"}", &cmd);
    assert(rc == 0);
    assert(cmd.type == MD_FIPSNAT_CMD_SHUTDOWN);
}

static void test_parse_punch(void) {
    const char *json =
        "{\"cmd\":\"punch\",\"peer_ip\":\"192.168.1.100\",\"peer_port\":9999,"
        "\"session_id\":\"aabbccdd11223344aabbccdd11223344\",\"timeout_ms\":5000}";

    MdFipsnatCmd cmd;
    int rc = md_fipsnat_ipc_parse_command(json, &cmd);
    assert(rc == 0);
    assert(cmd.type == MD_FIPSNAT_CMD_PUNCH);
    assert(strcmp(cmd.punch.peer_ip, "192.168.1.100") == 0);
    assert(cmd.punch.peer_port == 9999);
    assert(cmd.punch.timeout_ms == 5000);
    assert(cmd.punch.session_id[0] == 0xaa);
    assert(cmd.punch.session_id[1] == 0xbb);
    assert(cmd.punch.session_id[15] == 0x44);
}

static void test_parse_punch_missing_fields(void) {
    MdFipsnatCmd cmd;
    /* Missing peer_port */
    int rc = md_fipsnat_ipc_parse_command(
        "{\"cmd\":\"punch\",\"peer_ip\":\"1.2.3.4\","
        "\"session_id\":\"00000000000000000000000000000000\"}", &cmd);
    assert(rc == -1);
}

static void test_parse_punch_bad_session_id(void) {
    MdFipsnatCmd cmd;
    /* Too short session_id */
    int rc = md_fipsnat_ipc_parse_command(
        "{\"cmd\":\"punch\",\"peer_ip\":\"1.2.3.4\",\"peer_port\":1234,"
        "\"session_id\":\"aabb\"}", &cmd);
    assert(rc == -1);
}

static void test_parse_unknown_command(void) {
    MdFipsnatCmd cmd;
    int rc = md_fipsnat_ipc_parse_command("{\"cmd\":\"frobnicate\"}", &cmd);
    assert(rc == -1);
}

static void test_parse_invalid_json(void) {
    MdFipsnatCmd cmd;
    assert(md_fipsnat_ipc_parse_command("not json", &cmd) == -1);
    assert(md_fipsnat_ipc_parse_command("{}", &cmd) == -1);
    assert(md_fipsnat_ipc_parse_command(NULL, &cmd) == -1);
    assert(md_fipsnat_ipc_parse_command("{\"cmd\":\"status\"}", NULL) == -1);
}

/* ── Command roundtrip tests ─────────────────────────────────── */

static void test_roundtrip_status(void) {
    char *json = md_fipsnat_ipc_cmd_status();
    MdFipsnatCmd cmd;
    assert(md_fipsnat_ipc_parse_command(json, &cmd) == 0);
    assert(cmd.type == MD_FIPSNAT_CMD_STATUS);
    free(json);
}

static void test_roundtrip_punch(void) {
    uint8_t sid[16] = {0xde,0xad,0xbe,0xef,0xca,0xfe,0xba,0xbe,
                       0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
    char *json = md_fipsnat_ipc_cmd_punch("2001:db8::1", 443, sid, 20000);
    assert(json);

    MdFipsnatCmd cmd;
    assert(md_fipsnat_ipc_parse_command(json, &cmd) == 0);
    assert(cmd.type == MD_FIPSNAT_CMD_PUNCH);
    assert(strcmp(cmd.punch.peer_ip, "2001:db8::1") == 0);
    assert(cmd.punch.peer_port == 443);
    assert(cmd.punch.timeout_ms == 20000);
    assert(memcmp(cmd.punch.session_id, sid, 16) == 0);
    free(json);
}

/* ── Response building tests ─────────���───────────────────────── */

static void test_error_response(void) {
    char *json = md_fipsnat_ipc_error_response("something broke");
    assert(json);
    assert(strstr(json, "\"ok\":false"));
    assert(strstr(json, "\"error\":\"something broke\""));
    assert(!md_fipsnat_ipc_response_ok(json));

    char *err = md_fipsnat_ipc_response_error(json);
    assert(err);
    assert(strcmp(err, "something broke") == 0);
    free(err);
    free(json);
}

static void test_ok_response(void) {
    char *json = md_fipsnat_ipc_ok_response("all good");
    assert(json);
    assert(strstr(json, "\"ok\":true"));
    assert(md_fipsnat_ipc_response_ok(json));
    free(json);
}

static void test_status_response(void) {
    MdNatEndpoint ep = {
        .stun = { .port = 12345, .is_ipv6 = false },
        .fips_port = 2121,
        .punch_port = 0,
    };
    strncpy(ep.stun.ip, "97.113.230.17", sizeof(ep.stun.ip));

    char *json = md_fipsnat_ipc_status_response(true, true, &ep);
    assert(json);
    assert(md_fipsnat_ipc_response_ok(json));

    MdFipsnatStatusResp st;
    assert(md_fipsnat_ipc_parse_status_response(json, &st) == 0);
    assert(st.stun_ok == true);
    assert(st.published == true);
    assert(strcmp(st.ip, "97.113.230.17") == 0);
    assert(st.port == 12345);
    assert(st.fips_port == 2121);
    assert(st.punch_port == 0);
    free(json);
}

static void test_status_response_no_stun(void) {
    char *json = md_fipsnat_ipc_status_response(false, false, NULL);
    assert(json);
    assert(md_fipsnat_ipc_response_ok(json));

    MdFipsnatStatusResp st;
    assert(md_fipsnat_ipc_parse_status_response(json, &st) == 0);
    assert(st.stun_ok == false);
    assert(st.published == false);
    assert(st.ip[0] == '\0');
    free(json);
}

static void test_discover_response(void) {
    MdStunResult stun = { .port = 50086, .is_ipv6 = false };
    strncpy(stun.ip, "203.0.113.99", sizeof(stun.ip));

    char *json = md_fipsnat_ipc_discover_response(&stun);
    assert(json);
    assert(md_fipsnat_ipc_response_ok(json));
    assert(strstr(json, "\"ip\":\"203.0.113.99\""));
    assert(strstr(json, "\"port\":50086"));
    free(json);
}

static void test_punch_response(void) {
    MdPunchResult result = {
        .fd = 7,
        .peer_port = 5678,
        .local_port = 19800,
        .rtt_ms = 42,
    };
    strncpy(result.peer_ip, "10.0.0.5", sizeof(result.peer_ip));

    char *json = md_fipsnat_ipc_punch_response(&result);
    assert(json);
    assert(md_fipsnat_ipc_response_ok(json));

    MdFipsnatPunchResp pr;
    assert(md_fipsnat_ipc_parse_punch_response(json, &pr) == 0);
    assert(pr.fd == 7);
    assert(strcmp(pr.peer_ip, "10.0.0.5") == 0);
    assert(pr.peer_port == 5678);
    assert(pr.local_port == 19800);
    assert(pr.rtt_ms == 42);
    free(json);
}

/* ── Response parsing edge cases ─────────────────────────────── */

static void test_response_ok_null_safety(void) {
    assert(md_fipsnat_ipc_response_ok(NULL) == false);
    assert(md_fipsnat_ipc_response_ok("not json") == false);
    assert(md_fipsnat_ipc_response_error(NULL) == NULL);
}

static void test_parse_punch_response_null_safety(void) {
    MdFipsnatPunchResp pr;
    assert(md_fipsnat_ipc_parse_punch_response(NULL, &pr) == -1);
    assert(md_fipsnat_ipc_parse_punch_response("{\"ok\":true}", NULL) == -1);
}

static void test_parse_status_response_null_safety(void) {
    MdFipsnatStatusResp st;
    assert(md_fipsnat_ipc_parse_status_response(NULL, &st) == -1);
    assert(md_fipsnat_ipc_parse_status_response("{\"ok\":true}", NULL) == -1);
}

static void test_parse_punch_response_error(void) {
    MdFipsnatPunchResp pr;
    /* ok:false should cause parse to fail */
    assert(md_fipsnat_ipc_parse_punch_response(
        "{\"ok\":false,\"error\":\"timeout\"}", &pr) == -1);
}

static void test_discover_response_null(void) {
    char *json = md_fipsnat_ipc_discover_response(NULL);
    assert(json);
    assert(!md_fipsnat_ipc_response_ok(json));
    free(json);
}

static void test_punch_response_null(void) {
    char *json = md_fipsnat_ipc_punch_response(NULL);
    assert(json);
    assert(!md_fipsnat_ipc_response_ok(json));
    free(json);
}

/* ── Full protocol roundtrip ─────────────────────────────────── */

static void test_full_roundtrip_punch(void) {
    /* Host builds command */
    uint8_t sid[16];
    for (int i = 0; i < 16; i++) sid[i] = (uint8_t)(i * 17);
    char *cmd_json = md_fipsnat_ipc_cmd_punch("198.51.100.1", 8080, sid, 10000);
    assert(cmd_json);

    /* Daemon parses command */
    MdFipsnatCmd cmd;
    assert(md_fipsnat_ipc_parse_command(cmd_json, &cmd) == 0);
    assert(cmd.type == MD_FIPSNAT_CMD_PUNCH);
    assert(strcmp(cmd.punch.peer_ip, "198.51.100.1") == 0);
    assert(cmd.punch.peer_port == 8080);
    assert(memcmp(cmd.punch.session_id, sid, 16) == 0);

    /* Daemon builds response */
    MdPunchResult result = {
        .fd = 42,
        .peer_port = 8080,
        .local_port = 54321,
        .rtt_ms = 15,
    };
    strncpy(result.peer_ip, "198.51.100.1", sizeof(result.peer_ip));
    char *resp_json = md_fipsnat_ipc_punch_response(&result);
    assert(resp_json);

    /* Host parses response */
    assert(md_fipsnat_ipc_response_ok(resp_json));
    MdFipsnatPunchResp pr;
    assert(md_fipsnat_ipc_parse_punch_response(resp_json, &pr) == 0);
    assert(pr.fd == 42);
    assert(strcmp(pr.peer_ip, "198.51.100.1") == 0);
    assert(pr.peer_port == 8080);
    assert(pr.local_port == 54321);
    assert(pr.rtt_ms == 15);

    free(cmd_json);
    free(resp_json);
}

/* ── Session ID hex encoding edge cases ──────────────────────── */

static void test_session_id_all_ff(void) {
    uint8_t sid[16];
    memset(sid, 0xff, 16);
    char *json = md_fipsnat_ipc_cmd_punch("10.0.0.1", 1, sid, 0);
    assert(json);
    assert(strstr(json, "\"session_id\":\"ffffffffffffffffffffffffffffffff\""));

    MdFipsnatCmd cmd;
    assert(md_fipsnat_ipc_parse_command(json, &cmd) == 0);
    assert(memcmp(cmd.punch.session_id, sid, 16) == 0);
    free(json);
}

static void test_session_id_all_zero(void) {
    uint8_t sid[16] = {0};
    char *json = md_fipsnat_ipc_cmd_punch("10.0.0.1", 1, sid, 0);
    assert(json);
    assert(strstr(json, "\"session_id\":\"00000000000000000000000000000000\""));

    MdFipsnatCmd cmd;
    assert(md_fipsnat_ipc_parse_command(json, &cmd) == 0);
    assert(memcmp(cmd.punch.session_id, sid, 16) == 0);
    free(json);
}

/* ── Port edge cases ─────────────────────────────────────────── */

static void test_port_edge_cases(void) {
    /* Port 0 */
    MdPunchResult r0 = { .peer_port = 0, .local_port = 0 };
    strncpy(r0.peer_ip, "10.0.0.1", sizeof(r0.peer_ip));
    char *j0 = md_fipsnat_ipc_punch_response(&r0);
    assert(j0);
    MdFipsnatPunchResp p0;
    assert(md_fipsnat_ipc_parse_punch_response(j0, &p0) == 0);
    assert(p0.peer_port == 0);
    free(j0);

    /* Port 65535 */
    MdPunchResult r65 = { .peer_port = 65535, .local_port = 65535 };
    strncpy(r65.peer_ip, "10.0.0.1", sizeof(r65.peer_ip));
    char *j65 = md_fipsnat_ipc_punch_response(&r65);
    assert(j65);
    MdFipsnatPunchResp p65;
    assert(md_fipsnat_ipc_parse_punch_response(j65, &p65) == 0);
    assert(p65.peer_port == 65535);
    assert(p65.local_port == 65535);
    free(j65);
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("fips-nat IPC protocol tests\n");
    printf("==============================\n\n");

    /* Command building */
    RUN_TEST(test_cmd_status);
    RUN_TEST(test_cmd_discover);
    RUN_TEST(test_cmd_shutdown);
    RUN_TEST(test_cmd_punch);
    RUN_TEST(test_cmd_punch_no_timeout);
    RUN_TEST(test_cmd_punch_null_safety);

    /* Command parsing */
    RUN_TEST(test_parse_status);
    RUN_TEST(test_parse_discover);
    RUN_TEST(test_parse_shutdown);
    RUN_TEST(test_parse_punch);
    RUN_TEST(test_parse_punch_missing_fields);
    RUN_TEST(test_parse_punch_bad_session_id);
    RUN_TEST(test_parse_unknown_command);
    RUN_TEST(test_parse_invalid_json);

    /* Command roundtrip */
    RUN_TEST(test_roundtrip_status);
    RUN_TEST(test_roundtrip_punch);

    /* Response building + parsing */
    RUN_TEST(test_error_response);
    RUN_TEST(test_ok_response);
    RUN_TEST(test_status_response);
    RUN_TEST(test_status_response_no_stun);
    RUN_TEST(test_discover_response);
    RUN_TEST(test_punch_response);

    /* Response parsing edge cases */
    RUN_TEST(test_response_ok_null_safety);
    RUN_TEST(test_parse_punch_response_null_safety);
    RUN_TEST(test_parse_status_response_null_safety);
    RUN_TEST(test_parse_punch_response_error);
    RUN_TEST(test_discover_response_null);
    RUN_TEST(test_punch_response_null);

    /* Full protocol roundtrip */
    RUN_TEST(test_full_roundtrip_punch);

    /* Session ID edge cases */
    RUN_TEST(test_session_id_all_ff);
    RUN_TEST(test_session_id_all_zero);

    /* Port edge cases */
    RUN_TEST(test_port_edge_cases);

    printf("\n==============================\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
