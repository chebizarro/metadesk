/*
 * metadesk — test_jsonrpc.c
 * Unit tests for JSON-RPC 2.0 message layer.
 */
#include "jsonrpc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASS(name) printf("  PASS  %s\n", name)

/* ── Parse tests ─────────────────────────────────────────────── */

static void test_parse_request_string_id(void)
{
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\","
                       "\"id\":\"abc\",\"params\":{}}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == 0);
    assert(req.id.type == MD_JSONRPC_ID_STRING);
    assert(strcmp(req.id.value.str, "abc") == 0);
    assert(strcmp(req.method, "tools/list") == 0);
    assert(req.params != NULL);
    assert(cJSON_IsObject(req.params));
    md_jsonrpc_request_free(&req);
    PASS("parse request with string id");
}

static void test_parse_request_number_id(void)
{
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":42}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == 0);
    assert(req.id.type == MD_JSONRPC_ID_NUMBER);
    assert(req.id.value.num == 42);
    assert(strcmp(req.method, "ping") == 0);
    assert(req.params == NULL);
    md_jsonrpc_request_free(&req);
    PASS("parse request with number id");
}

static void test_parse_request_null_id(void)
{
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":null}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == 0);
    assert(req.id.type == MD_JSONRPC_ID_NULL);
    assert(strcmp(req.method, "ping") == 0);
    md_jsonrpc_request_free(&req);
    PASS("parse request with null id");
}

static void test_parse_notification(void)
{
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\"}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == 0);
    assert(req.id.type == MD_JSONRPC_ID_NONE);
    assert(md_jsonrpc_is_notification(&req));
    assert(strcmp(req.method, "initialized") == 0);
    md_jsonrpc_request_free(&req);
    PASS("parse notification (no id)");
}

static void test_parse_with_array_params(void)
{
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"foo\","
                       "\"id\":1,\"params\":[1,2,3]}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == 0);
    assert(cJSON_IsArray(req.params));
    assert(cJSON_GetArraySize(req.params) == 3);
    md_jsonrpc_request_free(&req);
    PASS("parse request with array params");
}

static void test_reject_malformed_json(void)
{
    const char *json = "{not valid json";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == -1);
    PASS("reject malformed JSON");
}

static void test_reject_missing_jsonrpc(void)
{
    const char *json = "{\"method\":\"foo\",\"id\":1}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == -1);
    PASS("reject missing jsonrpc field");
}

static void test_reject_wrong_version(void)
{
    const char *json = "{\"jsonrpc\":\"1.0\",\"method\":\"foo\",\"id\":1}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == -1);
    PASS("reject wrong jsonrpc version");
}

static void test_reject_missing_method(void)
{
    const char *json = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == -1);
    PASS("reject missing method");
}

static void test_reject_invalid_params_type(void)
{
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"foo\","
                       "\"id\":1,\"params\":\"string\"}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == -1);
    PASS("reject non-object/array params");
}

static void test_reject_invalid_id_type(void)
{
    const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"foo\","
                       "\"id\":[1,2]}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, json, strlen(json)) == -1);
    PASS("reject array id");
}

/* ── Serialize tests ─────────────────────────────────────────── */

static void test_make_response_string_id(void)
{
    MdJsonRpcId id = { .type = MD_JSONRPC_ID_STRING, .value.str = "req-1" };
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");

    char *json = md_jsonrpc_make_response(&id, result);
    assert(json != NULL);

    /* Parse back and verify */
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    assert(strcmp(cJSON_GetObjectItem(root, "jsonrpc")->valuestring, "2.0") == 0);
    assert(strcmp(cJSON_GetObjectItem(root, "id")->valuestring, "req-1") == 0);
    assert(cJSON_IsObject(cJSON_GetObjectItem(root, "result")));
    assert(!cJSON_HasObjectItem(root, "error"));
    cJSON_Delete(root);
    free(json);
    PASS("make response with string id");
}

static void test_make_response_number_id(void)
{
    MdJsonRpcId id = { .type = MD_JSONRPC_ID_NUMBER, .value.num = 7 };
    char *json = md_jsonrpc_make_response(&id, cJSON_CreateNull());
    assert(json != NULL);

    cJSON *root = cJSON_Parse(json);
    assert(cJSON_GetObjectItem(root, "id")->valueint == 7);
    assert(cJSON_IsNull(cJSON_GetObjectItem(root, "result")));
    cJSON_Delete(root);
    free(json);
    PASS("make response with number id");
}

static void test_make_error(void)
{
    MdJsonRpcId id = { .type = MD_JSONRPC_ID_NUMBER, .value.num = 3 };
    char *json = md_jsonrpc_make_error_simple(&id, MD_JSONRPC_METHOD_NOT_FOUND,
                                              "Method not found");
    assert(json != NULL);

    cJSON *root = cJSON_Parse(json);
    cJSON *err = cJSON_GetObjectItem(root, "error");
    assert(err != NULL);
    assert(cJSON_GetObjectItem(err, "code")->valueint == -32601);
    assert(strcmp(cJSON_GetObjectItem(err, "message")->valuestring,
                 "Method not found") == 0);
    assert(!cJSON_HasObjectItem(root, "result"));
    cJSON_Delete(root);
    free(json);
    PASS("make error response");
}

static void test_make_error_with_data(void)
{
    MdJsonRpcId id = { .type = MD_JSONRPC_ID_STRING, .value.str = "x" };
    cJSON *data = cJSON_CreateString("extra info");
    MdJsonRpcError err = {
        .code = MD_JSONRPC_INVALID_PARAMS,
        .message = "Invalid params",
        .data = data,
    };
    char *json = md_jsonrpc_make_error(&id, &err);
    assert(json != NULL);

    cJSON *root = cJSON_Parse(json);
    cJSON *e = cJSON_GetObjectItem(root, "error");
    assert(cJSON_IsString(cJSON_GetObjectItem(e, "data")));
    assert(strcmp(cJSON_GetObjectItem(e, "data")->valuestring, "extra info") == 0);
    cJSON_Delete(root);
    free(json);
    PASS("make error with data field");
}

static void test_make_notification(void)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "uri", "metadesk://ui-tree");
    char *json = md_jsonrpc_make_notification("notifications/resources/updated",
                                              params);
    assert(json != NULL);

    cJSON *root = cJSON_Parse(json);
    assert(strcmp(cJSON_GetObjectItem(root, "jsonrpc")->valuestring, "2.0") == 0);
    assert(strcmp(cJSON_GetObjectItem(root, "method")->valuestring,
                 "notifications/resources/updated") == 0);
    assert(cJSON_IsObject(cJSON_GetObjectItem(root, "params")));
    assert(!cJSON_HasObjectItem(root, "id"));  /* notifications have no id */
    cJSON_Delete(root);
    free(json);
    PASS("make notification");
}

/* ── Round-trip test ─────────────────────────────────────────── */

static void test_roundtrip_id_match(void)
{
    /* Parse a request, then build a response with the same id */
    const char *req_json = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\","
                           "\"id\":\"req-42\",\"params\":{\"name\":\"click\"}}";
    MdJsonRpcRequest req;
    assert(md_jsonrpc_parse_request(&req, req_json, strlen(req_json)) == 0);

    /* Build response using the parsed request's id */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "done", "yes");
    char *resp_json = md_jsonrpc_make_response(&req.id, result);
    assert(resp_json != NULL);

    /* Verify the response id matches the request */
    cJSON *resp = cJSON_Parse(resp_json);
    assert(strcmp(cJSON_GetObjectItem(resp, "id")->valuestring, "req-42") == 0);

    cJSON_Delete(resp);
    free(resp_json);
    md_jsonrpc_request_free(&req);
    PASS("round-trip id match");
}

/* ── ID copy/free ────────────────────────────────────────────── */

static void test_id_copy(void)
{
    MdJsonRpcId orig = { .type = MD_JSONRPC_ID_STRING, .value.str = strdup("hello") };
    MdJsonRpcId copy = md_jsonrpc_id_copy(&orig);

    assert(copy.type == MD_JSONRPC_ID_STRING);
    assert(strcmp(copy.value.str, "hello") == 0);
    assert(copy.value.str != orig.value.str);  /* must be independent */

    md_jsonrpc_id_free(&copy);
    assert(copy.type == MD_JSONRPC_ID_NONE);
    md_jsonrpc_id_free(&orig);
    PASS("id copy and free");
}

/* ── NULL safety ─────────────────────────────────────────────── */

static void test_null_safety(void)
{
    assert(md_jsonrpc_parse_request(NULL, "{}", 2) == -1);
    assert(md_jsonrpc_parse_request(&(MdJsonRpcRequest){0}, NULL, 0) == -1);
    assert(md_jsonrpc_make_response(NULL, NULL) != NULL);  /* null id → "id":null */
    assert(md_jsonrpc_make_notification(NULL, NULL) == NULL);

    /* Free with NULL should not crash */
    md_jsonrpc_request_free(NULL);
    md_jsonrpc_id_free(NULL);
    PASS("null safety");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("JSON-RPC 2.0 tests:\n");

    /* Parse */
    test_parse_request_string_id();
    test_parse_request_number_id();
    test_parse_request_null_id();
    test_parse_notification();
    test_parse_with_array_params();
    test_reject_malformed_json();
    test_reject_missing_jsonrpc();
    test_reject_wrong_version();
    test_reject_missing_method();
    test_reject_invalid_params_type();
    test_reject_invalid_id_type();

    /* Serialize */
    test_make_response_string_id();
    test_make_response_number_id();
    test_make_error();
    test_make_error_with_data();
    test_make_notification();

    /* Round-trip */
    test_roundtrip_id_match();

    /* ID helpers */
    test_id_copy();

    /* Safety */
    test_null_safety();

    printf("\nAll JSON-RPC tests passed.\n");
    return 0;
}
