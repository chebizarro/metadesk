/*
 * metadesk — jsonrpc.h
 * Minimal JSON-RPC 2.0 message parser/serializer on top of cJSON.
 *
 * Provides parse/serialize for requests, responses, and notifications.
 * No batch support (MCP doesn't use it).
 */
#ifndef MD_JSONRPC_H
#define MD_JSONRPC_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Standard JSON-RPC 2.0 error codes ──────────────────────── */

#define MD_JSONRPC_PARSE_ERROR      (-32700)
#define MD_JSONRPC_INVALID_REQUEST  (-32600)
#define MD_JSONRPC_METHOD_NOT_FOUND (-32601)
#define MD_JSONRPC_INVALID_PARAMS   (-32602)
#define MD_JSONRPC_INTERNAL_ERROR   (-32603)

/* ── ID representation ───────────────────────────────────────── */

typedef enum {
    MD_JSONRPC_ID_NONE,    /* notification — no id field         */
    MD_JSONRPC_ID_STRING,  /* "id": "abc"                        */
    MD_JSONRPC_ID_NUMBER,  /* "id": 42                           */
    MD_JSONRPC_ID_NULL,    /* "id": null (valid per spec)        */
} MdJsonRpcIdType;

typedef struct {
    MdJsonRpcIdType type;
    union {
        char   *str;       /* owned string, freed by _request_free / _response_free */
        int     num;
    } value;
} MdJsonRpcId;

/* ── Parsed request ──────────────────────────────────────────── */

typedef struct {
    MdJsonRpcId  id;       /* ID_NONE for notifications          */
    char        *method;   /* owned string                       */
    cJSON       *params;   /* borrowed — owned by the parsed root, or NULL */
    cJSON       *_root;    /* internal: full parsed tree (caller must not touch) */
} MdJsonRpcRequest;

/* ── Error object ────────────────────────────────────────────── */

typedef struct {
    int          code;
    const char  *message;  /* static or caller-managed string    */
    cJSON       *data;     /* optional extra data, or NULL       */
} MdJsonRpcError;

/* ── Parse a JSON-RPC 2.0 request / notification ─────────────
 *
 * On success, fills `req` and returns 0.
 * On failure, returns -1 (malformed JSON or not a valid JSON-RPC 2.0 message).
 *
 * The caller MUST call md_jsonrpc_request_free() when done.
 * `req->params` points into the internal parse tree and is valid until free.
 */
int md_jsonrpc_parse_request(MdJsonRpcRequest *req,
                             const char *json, size_t json_len);

/* Free all resources held by a parsed request. */
void md_jsonrpc_request_free(MdJsonRpcRequest *req);

/* Check if the request is a notification (no id). */
static inline bool md_jsonrpc_is_notification(const MdJsonRpcRequest *req) {
    return req->id.type == MD_JSONRPC_ID_NONE;
}

/* ── Serialize a success response ────────────────────────────
 *
 * Returns a newly-allocated JSON string. Caller must free().
 * `result` is consumed (added to the response tree and freed with it).
 * Pass cJSON_CreateNull() for a void-result response.
 */
char *md_jsonrpc_make_response(const MdJsonRpcId *id, cJSON *result);

/* ── Serialize an error response ─────────────────────────────
 *
 * Returns a newly-allocated JSON string. Caller must free().
 * `error->data` is consumed if non-NULL.
 */
char *md_jsonrpc_make_error(const MdJsonRpcId *id, const MdJsonRpcError *error);

/* Convenience: make an error response from code + message. */
char *md_jsonrpc_make_error_simple(const MdJsonRpcId *id,
                                   int code, const char *message);

/* ── Serialize a server → client notification ────────────────
 *
 * Returns a newly-allocated JSON string. Caller must free().
 * `params` is consumed (may be NULL for no-params notifications).
 */
char *md_jsonrpc_make_notification(const char *method, cJSON *params);

/* ── ID helpers ──────────────────────────────────────────────── */

/* Add "id" field to a cJSON object based on an MdJsonRpcId. */
void md_jsonrpc_id_to_json(cJSON *obj, const MdJsonRpcId *id);

/* Deep-copy an ID. Caller must free the copy with md_jsonrpc_id_free(). */
MdJsonRpcId md_jsonrpc_id_copy(const MdJsonRpcId *id);

/* Free an ID's owned string (if any). */
void md_jsonrpc_id_free(MdJsonRpcId *id);

#ifdef __cplusplus
}
#endif

#endif /* MD_JSONRPC_H */
