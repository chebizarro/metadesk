/*
 * metadesk — mcp_server.h
 * MCP (Model Context Protocol) server core.
 *
 * Transport-agnostic: the server reads/writes JSON strings via callbacks.
 * Handles initialize handshake, method dispatch, tool/resource registration.
 */
#ifndef MD_MCP_SERVER_H
#define MD_MCP_SERVER_H

#include "jsonrpc.h"
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declarations ────────────────────────────────────── */

typedef struct MdMcpServer MdMcpServer;

/* ── Transport write callback ────────────────────────────────── */

/* Called by the server to send a JSON string to the client.
 * `json` is a null-terminated string. `len` is its byte length.
 * Returns 0 on success, -1 on error. */
typedef int (*MdMcpWriteFn)(const char *json, size_t len, void *userdata);

/* ── Tool definition ─────────────────────────────────────────── */

/* Callback invoked when tools/call is received for this tool.
 * `arguments` is the parsed params.arguments object (may be NULL).
 * Must return a cJSON object representing the tool result content array,
 * or NULL on error (sets *is_error = true and *error_msg).
 * The returned cJSON is consumed by the server. */
typedef cJSON *(*MdMcpToolHandlerFn)(const cJSON *arguments,
                                     bool *is_error,
                                     char **error_msg,
                                     void *userdata);

typedef struct {
    const char         *name;         /* tool name, e.g. "metadesk_click" */
    const char         *description;  /* human/LLM-readable description   */
    cJSON              *input_schema; /* JSON Schema for arguments (owned) */
    MdMcpToolHandlerFn  handler;
    void               *userdata;
} MdMcpTool;

/* ── Resource definition ─────────────────────────────────────── */

/* Callback invoked when resources/read is received for this resource.
 * Must return a cJSON array of content objects [{type, text/data, ...}].
 * The returned cJSON is consumed by the server. */
typedef cJSON *(*MdMcpResourceReadFn)(void *userdata);

typedef struct {
    const char          *uri;          /* e.g. "metadesk://ui-tree"      */
    const char          *name;         /* display name                    */
    const char          *description;  /* human-readable description      */
    const char          *mime_type;    /* e.g. "application/json"         */
    MdMcpResourceReadFn  read_handler;
    void                *userdata;
} MdMcpResource;

/* ── Server configuration ────────────────────────────────────── */

typedef struct {
    const char   *server_name;     /* e.g. "metadesk"              */
    const char   *server_version;  /* e.g. "0.1.0"                 */
    MdMcpWriteFn  write_fn;        /* transport write callback      */
    void         *write_userdata;
} MdMcpServerConfig;

/* ── Capacity limits ─────────────────────────────────────────── */

#define MD_MCP_MAX_TOOLS       32
#define MD_MCP_MAX_RESOURCES   16
#define MD_MCP_MAX_SUBSCRIPTIONS 16

/* ── Server lifecycle ────────────────────────────────────────── */

MdMcpServer *md_mcp_server_create(const MdMcpServerConfig *config);
void         md_mcp_server_destroy(MdMcpServer *server);

/* ── Registration ────────────────────────────────────────────── */

/* Register a tool. The server copies the MdMcpTool struct but takes
 * ownership of input_schema. Returns 0 on success. */
int md_mcp_server_register_tool(MdMcpServer *server, const MdMcpTool *tool);

/* Unregister a tool by name. Deletes the owned input_schema for that tool.
 * Returns 0 if removed, -1 if not found or invalid. */
int md_mcp_server_unregister_tool(MdMcpServer *server, const char *name);

/* Register a resource. Returns 0 on success. */
int md_mcp_server_register_resource(MdMcpServer *server, const MdMcpResource *res);

/* ── Message handling ────────────────────────────────────────── */

/* Handle an incoming JSON message from the transport.
 * Parses, dispatches, and sends the response via write_fn.
 * Returns 0 on success, -1 on error. */
int md_mcp_server_handle_message(MdMcpServer *server,
                                 const char *json, size_t len);

/* ── Notifications ───────────────────────────────────────────── */

/* Send a resource-updated notification to the client.
 * Only sent if the client has subscribed to this URI. */
int md_mcp_server_notify_resource_updated(MdMcpServer *server,
                                          const char *uri);

/* ── State query ─────────────────────────────────────────────── */

bool md_mcp_server_is_initialized(const MdMcpServer *server);

#ifdef __cplusplus
}
#endif

#endif /* MD_MCP_SERVER_H */
