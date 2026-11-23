/*
 * test_session_log.c — tests for signed Nostr session log (M2.4).
 *
 * Unit tests for the local ring and a controlled loopback relay integration test.
 */
#include "session_log.h"
#include "nostr.h"
#include "signer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cjson/cJSON.h>
#include <json.h>
#include "md_test.h"

/* ── Helper: parse and validate content JSON ─────────────────── */

static cJSON *parse_content(const char *json) {
    MD_CHECK(json != NULL);
    cJSON *root = cJSON_Parse(json);
    MD_CHECK(root != NULL);
    return root;
}

/* ── Test: event type names ──────────────────────────────────── */

static void test_event_names(void) {
    MD_CHECK(strcmp(md_session_log_event_name(MD_SESSION_LOG_CONNECT), "connect") == 0);
    MD_CHECK(strcmp(md_session_log_event_name(MD_SESSION_LOG_DISCONNECT), "disconnect") == 0);
    MD_CHECK(strcmp(md_session_log_event_name(MD_SESSION_LOG_ACTION), "action") == 0);
    MD_CHECK(strcmp(md_session_log_event_name(MD_SESSION_LOG_REQUEST), "request") == 0);
    MD_CHECK(strcmp(md_session_log_event_name(MD_SESSION_LOG_ACCEPT), "accept") == 0);
    MD_CHECK(strcmp(md_session_log_event_name(MD_SESSION_LOG_DENY), "deny") == 0);
    MD_CHECK(strcmp(md_session_log_event_name((MdSessionLogEventType)99), "unknown") == 0);
    printf("  PASS event_names\n");
}

/* ── Test: build_content JSON structure ───────��──────────────── */

static void test_build_content(void) {
    char *json = md_session_log_build_content(
        MD_SESSION_LOG_CONNECT,
        "abc-123",
        "deadbeef01234567",
        "client connected via TCP",
        1700000000);
    MD_CHECK(json != NULL);

    cJSON *root = parse_content(json);
    MD_CHECK(strcmp(cJSON_GetObjectItem(root, "type")->valuestring,
                 "session_log") == 0);
    MD_CHECK(strcmp(cJSON_GetObjectItem(root, "event")->valuestring,
                 "connect") == 0);
    MD_CHECK((int64_t)cJSON_GetObjectItem(root, "ts")->valuedouble == 1700000000);
    MD_CHECK(strcmp(cJSON_GetObjectItem(root, "session_id")->valuestring,
                 "abc-123") == 0);
    MD_CHECK(strcmp(cJSON_GetObjectItem(root, "peer")->valuestring,
                 "deadbeef01234567") == 0);
    MD_CHECK(strcmp(cJSON_GetObjectItem(root, "detail")->valuestring,
                 "client connected via TCP") == 0);

    cJSON_Delete(root);
    free(json);

    /* Test with NULL optional fields */
    json = md_session_log_build_content(MD_SESSION_LOG_DENY, NULL, NULL, NULL, 0);
    MD_CHECK(json != NULL);
    root = parse_content(json);
    MD_CHECK(strcmp(cJSON_GetObjectItem(root, "event")->valuestring, "deny") == 0);
    MD_CHECK(cJSON_GetObjectItem(root, "session_id") == NULL);
    MD_CHECK(cJSON_GetObjectItem(root, "peer") == NULL);
    MD_CHECK(cJSON_GetObjectItem(root, "detail") == NULL);
    cJSON_Delete(root);
    free(json);

    printf("  PASS build_content\n");
}

/* ── Test: create / destroy lifecycle ────────────────��───────── */

static void test_lifecycle(void) {
    /* Default config */
    MdSessionLogConfig cfg = { 0 };
    MdSessionLog *log = md_session_log_create(&cfg);
    MD_CHECK(log != NULL);
    MD_CHECK(md_session_log_count(log) == 0);
    md_session_log_destroy(log);

    /* NULL config */
    log = md_session_log_create(NULL);
    MD_CHECK(log != NULL);
    md_session_log_destroy(log);

    /* Custom capacity */
    MdSessionLogConfig cfg2 = { .capacity = 4 };
    log = md_session_log_create(&cfg2);
    MD_CHECK(log != NULL);
    MD_CHECK(md_session_log_count(log) == 0);
    md_session_log_destroy(log);

    /* Destroy NULL is safe */
    md_session_log_destroy(NULL);

    printf("  PASS lifecycle\n");
}

/* ── Test: log events and query ──────────────────────────────── */

static void test_log_and_query(void) {
    MdSessionLogConfig cfg = { .capacity = 8 };
    MdSessionLog *log = md_session_log_create(&cfg);
    MD_CHECK(log != NULL);

    /* Log a connect event */
    int ret = md_session_log_event(log, MD_SESSION_LOG_CONNECT,
                                   "sess-001",
                                   "aabbccdd11223344",
                                   "TCP client connected");
    MD_CHECK(ret == 0);
    MD_CHECK(md_session_log_count(log) == 1);

    /* Query the entry */
    const MdSessionLogEntry *e = md_session_log_get(log, 0);
    MD_CHECK(e != NULL);
    MD_CHECK(e->type == MD_SESSION_LOG_CONNECT);
    MD_CHECK(strcmp(e->session_id, "sess-001") == 0);
    MD_CHECK(strcmp(e->peer_pubkey, "aabbccdd11223344") == 0);
    MD_CHECK(strcmp(e->detail, "TCP client connected") == 0);
    MD_CHECK(e->timestamp > 0);
    /* No signer configured, so signed_json should be NULL */
    MD_CHECK(e->signed_json == NULL);

    /* Log more events */
    md_session_log_event(log, MD_SESSION_LOG_ACTION, "sess-001",
                         "aabbccdd11223344", "click(500,300)");
    md_session_log_event(log, MD_SESSION_LOG_DISCONNECT, "sess-001",
                         "aabbccdd11223344", "client disconnected");
    MD_CHECK(md_session_log_count(log) == 3);

    /* Verify ordering (0 = oldest) */
    e = md_session_log_get(log, 0);
    MD_CHECK(e->type == MD_SESSION_LOG_CONNECT);
    e = md_session_log_get(log, 1);
    MD_CHECK(e->type == MD_SESSION_LOG_ACTION);
    e = md_session_log_get(log, 2);
    MD_CHECK(e->type == MD_SESSION_LOG_DISCONNECT);

    /* Out of range returns NULL */
    MD_CHECK(md_session_log_get(log, 3) == NULL);
    MD_CHECK(md_session_log_get(log, -1) == NULL);

    md_session_log_destroy(log);
    printf("  PASS log_and_query\n");
}

/* ── Test: ring buffer wrapping ────────��─────────────────────── */

static void test_ring_buffer_wrap(void) {
    MdSessionLogConfig cfg = { .capacity = 4 };
    MdSessionLog *log = md_session_log_create(&cfg);
    MD_CHECK(log != NULL);

    /* Fill the buffer exactly */
    char detail[32];
    for (int i = 0; i < 4; i++) {
        snprintf(detail, sizeof(detail), "event-%d", i);
        md_session_log_event(log, MD_SESSION_LOG_ACTION, NULL, NULL, detail);
    }
    MD_CHECK(md_session_log_count(log) == 4);

    /* Verify all four entries */
    for (int i = 0; i < 4; i++) {
        snprintf(detail, sizeof(detail), "event-%d", i);
        const MdSessionLogEntry *e = md_session_log_get(log, i);
        MD_CHECK(e != NULL);
        MD_CHECK(strcmp(e->detail, detail) == 0);
    }

    /* Add one more — should wrap, dropping event-0 */
    md_session_log_event(log, MD_SESSION_LOG_ACTION, NULL, NULL, "event-4");
    MD_CHECK(md_session_log_count(log) == 4);  /* capped at capacity */

    /* Oldest should now be event-1 */
    const MdSessionLogEntry *e = md_session_log_get(log, 0);
    MD_CHECK(e != NULL);
    MD_CHECK(strcmp(e->detail, "event-1") == 0);

    /* Newest (index 3) should be event-4 */
    e = md_session_log_get(log, 3);
    MD_CHECK(e != NULL);
    MD_CHECK(strcmp(e->detail, "event-4") == 0);

    /* Add several more to wrap multiple times */
    md_session_log_event(log, MD_SESSION_LOG_ACTION, NULL, NULL, "event-5");
    md_session_log_event(log, MD_SESSION_LOG_ACTION, NULL, NULL, "event-6");
    md_session_log_event(log, MD_SESSION_LOG_ACTION, NULL, NULL, "event-7");
    MD_CHECK(md_session_log_count(log) == 4);

    /* Should have events 4,5,6,7 */
    e = md_session_log_get(log, 0);
    MD_CHECK(strcmp(e->detail, "event-4") == 0);
    e = md_session_log_get(log, 3);
    MD_CHECK(strcmp(e->detail, "event-7") == 0);

    md_session_log_destroy(log);
    printf("  PASS ring_buffer_wrap\n");
}

/* ── Test: null safety ──────────────��────────────────────────── */

static void test_null_safety(void) {
    /* All APIs handle NULL gracefully */
    MD_CHECK(md_session_log_count(NULL) == 0);
    MD_CHECK(md_session_log_get(NULL, 0) == NULL);
    MD_CHECK(md_session_log_event(NULL, MD_SESSION_LOG_CONNECT,
                                NULL, NULL, NULL) == -1);

    /* Log with all-NULL optional params */
    MdSessionLogConfig cfg = { .capacity = 4 };
    MdSessionLog *log = md_session_log_create(&cfg);
    MD_CHECK(log != NULL);

    int ret = md_session_log_event(log, MD_SESSION_LOG_DENY, NULL, NULL, NULL);
    MD_CHECK(ret == 0);
    MD_CHECK(md_session_log_count(log) == 1);

    const MdSessionLogEntry *e = md_session_log_get(log, 0);
    MD_CHECK(e != NULL);
    MD_CHECK(e->type == MD_SESSION_LOG_DENY);
    MD_CHECK(e->session_id[0] == '\0');
    MD_CHECK(e->peer_pubkey[0] == '\0');
    MD_CHECK(e->detail[0] == '\0');

    md_session_log_destroy(log);
    printf("  PASS null_safety\n");
}

/* ── Test: all event types can be logged ─────────────────────── */

static void test_all_event_types(void) {
    MdSessionLogConfig cfg = { .capacity = 16 };
    MdSessionLog *log = md_session_log_create(&cfg);

    MdSessionLogEventType types[] = {
        MD_SESSION_LOG_CONNECT,
        MD_SESSION_LOG_DISCONNECT,
        MD_SESSION_LOG_ACTION,
        MD_SESSION_LOG_REQUEST,
        MD_SESSION_LOG_ACCEPT,
        MD_SESSION_LOG_DENY,
    };
    int n = sizeof(types) / sizeof(types[0]);

    for (int i = 0; i < n; i++) {
        int ret = md_session_log_event(log, types[i], "sess", "peer", "test");
        MD_CHECK(ret == 0);
    }
    MD_CHECK(md_session_log_count(log) == n);

    /* Verify each entry has correct type */
    for (int i = 0; i < n; i++) {
        const MdSessionLogEntry *e = md_session_log_get(log, i);
        MD_CHECK(e != NULL);
        MD_CHECK(e->type == types[i]);
    }

    md_session_log_destroy(log);
    printf("  PASS all_event_types\n");
}

/* ── Test: build_content with empty strings ──────────────────── */

static void test_build_content_empty_strings(void) {
    /* Empty strings should be treated like NULL (omitted from JSON) */
    char *json = md_session_log_build_content(MD_SESSION_LOG_ACTION, "", "", "", 12345);
    MD_CHECK(json != NULL);

    cJSON *root = parse_content(json);
    MD_CHECK(cJSON_GetObjectItem(root, "session_id") == NULL);
    MD_CHECK(cJSON_GetObjectItem(root, "peer") == NULL);
    MD_CHECK(cJSON_GetObjectItem(root, "detail") == NULL);
    MD_CHECK(strcmp(cJSON_GetObjectItem(root, "event")->valuestring, "action") == 0);

    cJSON_Delete(root);
    free(json);
    printf("  PASS build_content_empty_strings\n");
}

/* ── Test: publish_signed_json null safety ────────────────────── */

static void test_nostr_publish_null_safety(void) {
    /* md_nostr_publish_signed_json(NULL, ...) should return -1 */
    MD_CHECK(md_nostr_publish_signed_json(NULL, "{}") == -1);
    MD_CHECK(md_nostr_publish_signed_json(NULL, NULL) == -1);
    printf("  PASS nostr_publish_null_safety\n");
}

/* ── Main ───────────────────���────────────────────────────────── */

static cJSON *relay_control(const char *command) {
    char reply[16384];
    printf("CONTROL %s\n", command);
    fflush(stdout);
    if (!fgets(reply, sizeof(reply), stdin)) exit(1);
    cJSON *result = cJSON_Parse(reply);
    if (!result) exit(1);
    return result;
}

static void relay_command(const char *command) {
    cJSON *result = relay_control(command);
    MD_CHECK(cJSON_IsString(result) && strcmp(result->valuestring, "OK") == 0);
    cJSON_Delete(result);
}

static void check_relay_copy(MdSigner *signer, const char *pk, const char *local) {
    cJSON *wire = relay_control("{\"op\":\"published\"}");
    char *json = cJSON_PrintUnformatted(wire);
    NostrEvent *ev = nostr_event_new();
    char id[65];
    if (!json || !ev || nostr_event_deserialize(ev, json) != 0 ||
        nostr_event_validate(ev, id) != NOSTR_EVENT_VALIDATION_OK) exit(1);
    MD_CHECK(nostr_event_get_kind(ev) == 1079);
    MD_CHECK(strcmp(nostr_event_get_pubkey(ev), pk) == 0);
    MD_CHECK(!strstr(json, "test-peer-pubkey"));
    MD_CHECK(!strstr(json, "test-private-session"));
    MD_CHECK(!strstr(json, "test-private-detail"));
    NostrTags *tags = nostr_event_get_tags(ev);
    if (nostr_tags_size(tags) != 1) exit(1);
    NostrTag *tag = nostr_tags_get(tags, 0);
    if (nostr_tag_size(tag) != 2) exit(1);
    MD_CHECK(strcmp(nostr_tag_get(tag, 0), "t") == 0);
    MD_CHECK(strcmp(nostr_tag_get(tag, 1), "metadesk-encrypted-session-log") == 0);
    char *plain = NULL;
    if (md_signer_nip44_decrypt(signer, pk, nostr_event_get_content(ev), &plain) != MD_SIGNER_OK) exit(1);
    MD_CHECK(strcmp(plain, local) == 0);
    free(plain);
    nostr_event_free(ev);
    free(json);
    cJSON_Delete(wire);
}

static void test_relay_publishing(const char *url) {
    MdSigner *signer = md_signer_create_direct(
        "0000000000000000000000000000000000000000000000000000000000000001");
    char *pk = NULL;
    if (!signer || md_signer_get_pubkey(signer, &pk) != MD_SIGNER_OK) exit(1);
    MdNostrConfig cfg = { .signer = signer, .relay_urls = &url, .relay_count = 1 };
    MdNostr *nostr = md_nostr_create(&cfg, NULL);
    if (!nostr) exit(1);
    relay_command("{\"op\":\"req\"}");
    relay_command("{\"op\":\"eose\"}");
    MdSessionLogConfig log_cfg = { .signer = signer, .nostr = nostr, .publish = true, .capacity = 1 };
    MdSessionLog *log = md_session_log_create(&log_cfg);
    if (!log) exit(1);

    /* The test driver is the only code that can release the relay's OK.
     * Synchronous publication cannot return to reach the ACK below. */
    MD_CHECK(md_session_log_event(log, MD_SESSION_LOG_REQUEST, "test-private-session",
                                  "test-peer-pubkey", "test-private-detail") == 0);
    const MdSessionLogEntry *first = md_session_log_get(log, 0);
    if (!first || !first->signed_json) exit(1);
    char *local = strdup(first->signed_json);
    if (!local) exit(1);
    relay_command("{\"op\":\"held\"}");

    /* Wrap the local ring while OK is still held. The queued copy owns its data. */
    MD_CHECK(md_session_log_event(log, MD_SESSION_LOG_ACCEPT, "test-private-session",
                                  "test-peer-pubkey", "test-private-detail") == 0);
    check_relay_copy(signer, pk, local);
    free(local);
    relay_command("{\"op\":\"ack\",\"ok\":false}");
    relay_command("{\"op\":\"held\"}");
    check_relay_copy(signer, pk, md_session_log_get(log, 0)->signed_json);
    relay_command("{\"op\":\"ack\",\"ok\":true}");
    md_session_log_destroy(log);
    md_nostr_destroy(nostr);
    free(pk);
    md_signer_destroy(signer);
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--acl-fixture") == 0) {
        test_relay_publishing(argv[2]);
        return md_test_report();
    }
    printf("test_session_log:\n");

    test_event_names();
    test_build_content();
    test_lifecycle();
    test_log_and_query();
    test_ring_buffer_wrap();
    test_null_safety();
    test_all_event_types();
    test_build_content_empty_strings();
    test_nostr_publish_null_safety();

    printf("  ALL PASSED\n");
    return md_test_report();
}
