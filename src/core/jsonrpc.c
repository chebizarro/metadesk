/*
 * metadesk — jsonrpc.c
 * Minimal JSON-RPC 2.0 message parser/serializer.
 */
#include "jsonrpc.h"
#include <stdlib.h>
#include <string.h>

/* ── ID helpers ──────────────────────────────────────────────── */

void md_jsonrpc_id_to_json(cJSON *obj, const MdJsonRpcId *id)
{
    if (!obj || !id) return;

    switch (id->type) {
    case MD_JSONRPC_ID_STRING:
        cJSON_AddStringToObject(obj, "id", id->value.str ? id->value.str : "");
        break;
    case MD_JSONRPC_ID_NUMBER:
        cJSON_AddNumberToObject(obj, "id", id->value.num);
        break;
    case MD_JSONRPC_ID_NULL:
        cJSON_AddNullToObject(obj, "id");
        break;
    case MD_JSONRPC_ID_NONE:
        /* notifications have no id field */
        break;
    }
}

MdJsonRpcId md_jsonrpc_id_copy(const MdJsonRpcId *id)
{
    MdJsonRpcId copy = {0};
    if (!id) return copy;

    copy.type = id->type;
    if (id->type == MD_JSONRPC_ID_STRING && id->value.str)
        copy.value.str = strdup(id->value.str);
    else if (id->type == MD_JSONRPC_ID_NUMBER)
        copy.value.num = id->value.num;

    return copy;
}

void md_jsonrpc_id_free(MdJsonRpcId *id)
{
    if (!id) return;
    if (id->type == MD_JSONRPC_ID_STRING) {
        free(id->value.str);
        id->value.str = NULL;
    }
    id->type = MD_JSONRPC_ID_NONE;
}

/* ── Parse request ───────────────────────────────────────────── */

int md_jsonrpc_parse_request(MdJsonRpcRequest *req,
                             const char *json, size_t json_len)
{
    if (!req || !json || json_len == 0)
        return -1;

    memset(req, 0, sizeof(*req));

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root)
        return -1;

    /* Must be an object */
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    /* "jsonrpc": "2.0" — required */
    cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
    if (!cJSON_IsString(ver) || strcmp(ver->valuestring, "2.0") != 0) {
        cJSON_Delete(root);
        return -1;
    }

    /* "method" — required, must be string */
    cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (!cJSON_IsString(method) || method->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return -1;
    }
    req->method = strdup(method->valuestring);
    if (!req->method) {
        cJSON_Delete(root);
        return -1;
    }

    /* "id" — optional (absent = notification) */
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!id) {
        req->id.type = MD_JSONRPC_ID_NONE;
    } else if (cJSON_IsString(id)) {
        req->id.type = MD_JSONRPC_ID_STRING;
        req->id.value.str = strdup(id->valuestring);
    } else if (cJSON_IsNumber(id)) {
        req->id.type = MD_JSONRPC_ID_NUMBER;
        req->id.value.num = id->valueint;
    } else if (cJSON_IsNull(id)) {
        req->id.type = MD_JSONRPC_ID_NULL;
    } else {
        /* id must be string, number, or null per spec */
        free(req->method);
        cJSON_Delete(root);
        return -1;
    }

    /* "params" — optional, must be object or array if present */
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (params && !cJSON_IsObject(params) && !cJSON_IsArray(params)) {
        md_jsonrpc_id_free(&req->id);
        free(req->method);
        cJSON_Delete(root);
        return -1;
    }
    req->params = params;  /* borrowed pointer into root */

    req->_root = root;
    return 0;
}

void md_jsonrpc_request_free(MdJsonRpcRequest *req)
{
    if (!req) return;

    free(req->method);
    req->method = NULL;

    md_jsonrpc_id_free(&req->id);

    if (req->_root) {
        cJSON_Delete(req->_root);
        req->_root = NULL;
    }
    req->params = NULL;
}

/* ── Serialize helpers ───────────────────────────────────────── */

/* Create the base {"jsonrpc":"2.0"} object. */
static cJSON *make_base(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "jsonrpc", "2.0");
    return obj;
}

/* Print and free a cJSON tree. Returns the JSON string (caller frees). */
static char *print_and_free(cJSON *obj)
{
    if (!obj) return NULL;
    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return str;
}

/* ── Success response ────────────────────────────────────────── */

char *md_jsonrpc_make_response(const MdJsonRpcId *id, cJSON *result)
{
    cJSON *obj = make_base();
    if (!obj) {
        if (result) cJSON_Delete(result);
        return NULL;
    }

    if (id)
        md_jsonrpc_id_to_json(obj, id);
    else
        cJSON_AddNullToObject(obj, "id");

    cJSON_AddItemToObject(obj, "result", result ? result : cJSON_CreateNull());

    return print_and_free(obj);
}

/* ── Error response ──────────────────────────────────────────── */

char *md_jsonrpc_make_error(const MdJsonRpcId *id, const MdJsonRpcError *error)
{
    if (!error) return NULL;

    cJSON *obj = make_base();
    if (!obj) return NULL;

    if (id)
        md_jsonrpc_id_to_json(obj, id);
    else
        cJSON_AddNullToObject(obj, "id");

    cJSON *err = cJSON_CreateObject();
    if (!err) {
        cJSON_Delete(obj);
        return NULL;
    }
    cJSON_AddNumberToObject(err, "code", error->code);
    cJSON_AddStringToObject(err, "message", error->message ? error->message : "");
    if (error->data)
        cJSON_AddItemToObject(err, "data", error->data);

    cJSON_AddItemToObject(obj, "error", err);

    return print_and_free(obj);
}

char *md_jsonrpc_make_error_simple(const MdJsonRpcId *id,
                                   int code, const char *message)
{
    MdJsonRpcError err = { .code = code, .message = message, .data = NULL };
    return md_jsonrpc_make_error(id, &err);
}

/* ── Server notification ─────────────────────────────────────── */

char *md_jsonrpc_make_notification(const char *method, cJSON *params)
{
    if (!method) {
        if (params) cJSON_Delete(params);
        return NULL;
    }

    cJSON *obj = make_base();
    if (!obj) {
        if (params) cJSON_Delete(params);
        return NULL;
    }

    cJSON_AddStringToObject(obj, "method", method);
    if (params)
        cJSON_AddItemToObject(obj, "params", params);

    return print_and_free(obj);
}
