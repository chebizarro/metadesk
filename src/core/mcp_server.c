/*
 * metadesk — mcp_server.c
 * MCP server core — method routing, capability handshake, tool/resource dispatch.
 */
#include "mcp_server.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── MCP protocol version ────────────────────────────────────── */

#define MCP_PROTOCOL_VERSION "2025-03-26"

/* ── Server struct ───────────────────────────────────────────── */

struct MdMcpServer {
    /* Config */
    char            *server_name;
    char            *server_version;
    MdMcpWriteFn     write_fn;
    void            *write_userdata;

    /* State */
    bool             initialized;
    pthread_mutex_t  mu;

    /* Registered tools */
    MdMcpTool        tools[MD_MCP_MAX_TOOLS];
    int              tool_count;

    /* Registered resources */
    MdMcpResource    resources[MD_MCP_MAX_RESOURCES];
    int              resource_count;

    /* Resource subscriptions (URIs the client has subscribed to) */
    char            *subscriptions[MD_MCP_MAX_SUBSCRIPTIONS];
    int              subscription_count;
};

/* ── Helpers ─────────────────────────────────────────────────── */

static int send_json(MdMcpServer *s, char *json)
{
    if (!json) return -1;
    int rc = s->write_fn(json, strlen(json), s->write_userdata);
    free(json);
    return rc;
}

static int send_error(MdMcpServer *s, const MdJsonRpcId *id,
                      int code, const char *message)
{
    return send_json(s, md_jsonrpc_make_error_simple(id, code, message));
}

/* ── Method handlers ─────────────────────────────────────────── */

static int handle_initialize(MdMcpServer *s, const MdJsonRpcId *id,
                             const cJSON *params)
{
    (void)params;  /* client info — logged but not used in Phase 1 */

    cJSON *result = cJSON_CreateObject();

    cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);

    /* Server info */
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", s->server_name);
    cJSON_AddStringToObject(info, "version", s->server_version);
    cJSON_AddItemToObject(result, "serverInfo", info);

    /* Capabilities */
    cJSON *caps = cJSON_CreateObject();
    if (s->tool_count > 0)
        cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    if (s->resource_count > 0) {
        cJSON *res_caps = cJSON_CreateObject();
        cJSON_AddBoolToObject(res_caps, "subscribe", true);
        cJSON_AddItemToObject(caps, "resources", res_caps);
    }
    cJSON_AddItemToObject(result, "capabilities", caps);

    return send_json(s, md_jsonrpc_make_response(id, result));
}

static int handle_initialized(MdMcpServer *s)
{
    /* "initialized" is a notification from the client — mark server ready */
    s->initialized = true;
    return 0;
}

static int handle_ping(MdMcpServer *s, const MdJsonRpcId *id)
{
    return send_json(s, md_jsonrpc_make_response(id, cJSON_CreateObject()));
}

static int handle_tools_list(MdMcpServer *s, const MdJsonRpcId *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();

    for (int i = 0; i < s->tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s->tools[i].name);
        if (s->tools[i].description)
            cJSON_AddStringToObject(tool, "description", s->tools[i].description);
        if (s->tools[i].input_schema)
            cJSON_AddItemToObject(tool, "inputSchema",
                                  cJSON_Duplicate(s->tools[i].input_schema, true));
        cJSON_AddItemToArray(tools, tool);
    }
    cJSON_AddItemToObject(result, "tools", tools);

    return send_json(s, md_jsonrpc_make_response(id, result));
}

static int handle_tools_call(MdMcpServer *s, const MdJsonRpcId *id,
                             const cJSON *params)
{
    if (!params) {
        return send_error(s, id, MD_JSONRPC_INVALID_PARAMS, "Missing params");
    }

    cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name)) {
        return send_error(s, id, MD_JSONRPC_INVALID_PARAMS,
                          "Missing or invalid 'name' in params");
    }

    /* Find the tool */
    MdMcpTool *tool = NULL;
    for (int i = 0; i < s->tool_count; i++) {
        if (strcmp(s->tools[i].name, name->valuestring) == 0) {
            tool = &s->tools[i];
            break;
        }
    }
    if (!tool) {
        return send_error(s, id, MD_JSONRPC_METHOD_NOT_FOUND,
                          "Unknown tool");
    }

    /* Extract arguments */
    cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");

    /* Call handler */
    bool is_error = false;
    char *error_msg = NULL;
    cJSON *content = tool->handler(arguments, &is_error, &error_msg,
                                   tool->userdata);

    /* Build result */
    cJSON *result = cJSON_CreateObject();
    if (content)
        cJSON_AddItemToObject(result, "content", content);
    else
        cJSON_AddItemToObject(result, "content", cJSON_CreateArray());

    if (is_error)
        cJSON_AddBoolToObject(result, "isError", true);

    free(error_msg);
    return send_json(s, md_jsonrpc_make_response(id, result));
}

static int handle_resources_list(MdMcpServer *s, const MdJsonRpcId *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *resources = cJSON_CreateArray();

    for (int i = 0; i < s->resource_count; i++) {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "uri", s->resources[i].uri);
        cJSON_AddStringToObject(res, "name", s->resources[i].name);
        if (s->resources[i].description)
            cJSON_AddStringToObject(res, "description",
                                    s->resources[i].description);
        if (s->resources[i].mime_type)
            cJSON_AddStringToObject(res, "mimeType",
                                    s->resources[i].mime_type);
        cJSON_AddItemToArray(resources, res);
    }
    cJSON_AddItemToObject(result, "resources", resources);

    return send_json(s, md_jsonrpc_make_response(id, result));
}

static int handle_resources_read(MdMcpServer *s, const MdJsonRpcId *id,
                                 const cJSON *params)
{
    if (!params) {
        return send_error(s, id, MD_JSONRPC_INVALID_PARAMS, "Missing params");
    }

    cJSON *uri = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (!cJSON_IsString(uri)) {
        return send_error(s, id, MD_JSONRPC_INVALID_PARAMS,
                          "Missing or invalid 'uri' in params");
    }

    /* Find the resource */
    MdMcpResource *res = NULL;
    for (int i = 0; i < s->resource_count; i++) {
        if (strcmp(s->resources[i].uri, uri->valuestring) == 0) {
            res = &s->resources[i];
            break;
        }
    }
    if (!res) {
        return send_error(s, id, MD_JSONRPC_INVALID_PARAMS,
                          "Unknown resource URI");
    }

    cJSON *contents = res->read_handler(res->userdata);

    cJSON *result = cJSON_CreateObject();
    if (contents)
        cJSON_AddItemToObject(result, "contents", contents);
    else
        cJSON_AddItemToObject(result, "contents", cJSON_CreateArray());

    return send_json(s, md_jsonrpc_make_response(id, result));
}

static int handle_resources_subscribe(MdMcpServer *s, const MdJsonRpcId *id,
                                      const cJSON *params)
{
    if (!params) {
        return send_error(s, id, MD_JSONRPC_INVALID_PARAMS, "Missing params");
    }

    cJSON *uri = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (!cJSON_IsString(uri)) {
        return send_error(s, id, MD_JSONRPC_INVALID_PARAMS,
                          "Missing or invalid 'uri'");
    }

    /* Check if already subscribed */
    for (int i = 0; i < s->subscription_count; i++) {
        if (strcmp(s->subscriptions[i], uri->valuestring) == 0) {
            return send_json(s, md_jsonrpc_make_response(id, cJSON_CreateObject()));
        }
    }

    if (s->subscription_count >= MD_MCP_MAX_SUBSCRIPTIONS) {
        return send_error(s, id, MD_JSONRPC_INTERNAL_ERROR,
                          "Too many subscriptions");
    }

    s->subscriptions[s->subscription_count++] = strdup(uri->valuestring);
    return send_json(s, md_jsonrpc_make_response(id, cJSON_CreateObject()));
}

static int handle_resources_unsubscribe(MdMcpServer *s, const MdJsonRpcId *id,
                                        const cJSON *params)
{
    if (!params) {
        return send_error(s, id, MD_JSONRPC_INVALID_PARAMS, "Missing params");
    }

    cJSON *uri = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (!cJSON_IsString(uri)) {
        return send_error(s, id, MD_JSONRPC_INVALID_PARAMS,
                          "Missing or invalid 'uri'");
    }

    for (int i = 0; i < s->subscription_count; i++) {
        if (strcmp(s->subscriptions[i], uri->valuestring) == 0) {
            free(s->subscriptions[i]);
            /* Shift remaining entries */
            for (int j = i; j < s->subscription_count - 1; j++)
                s->subscriptions[j] = s->subscriptions[j + 1];
            s->subscription_count--;
            break;
        }
    }

    return send_json(s, md_jsonrpc_make_response(id, cJSON_CreateObject()));
}

/* ── Main dispatch ───────────────────────────────────────────── */

int md_mcp_server_handle_message(MdMcpServer *server,
                                 const char *json, size_t len)
{
    if (!server || !json || len == 0)
        return -1;

    MdJsonRpcRequest req;
    if (md_jsonrpc_parse_request(&req, json, len) != 0) {
        /* Can't parse — send parse error with null id */
        MdJsonRpcId null_id = { .type = MD_JSONRPC_ID_NULL };
        send_error(server, &null_id, MD_JSONRPC_PARSE_ERROR, "Parse error");
        return -1;
    }

    int rc = 0;

    /* "initialized" notification — always allowed */
    if (strcmp(req.method, "notifications/initialized") == 0 ||
        strcmp(req.method, "initialized") == 0) {
        rc = handle_initialized(server);
        md_jsonrpc_request_free(&req);
        return rc;
    }

    /* "initialize" — always allowed (it's what sets initialized=true) */
    if (strcmp(req.method, "initialize") == 0) {
        rc = handle_initialize(server, &req.id, req.params);
        md_jsonrpc_request_free(&req);
        return rc;
    }

    /* All other methods require initialization */
    if (!server->initialized) {
        if (!md_jsonrpc_is_notification(&req)) {
            rc = send_error(server, &req.id, MD_JSONRPC_INTERNAL_ERROR,
                            "Server not initialized");
        }
        md_jsonrpc_request_free(&req);
        return rc;
    }

    /* Dispatch by method name */
    if (strcmp(req.method, "ping") == 0) {
        rc = handle_ping(server, &req.id);
    } else if (strcmp(req.method, "tools/list") == 0) {
        rc = handle_tools_list(server, &req.id);
    } else if (strcmp(req.method, "tools/call") == 0) {
        rc = handle_tools_call(server, &req.id, req.params);
    } else if (strcmp(req.method, "resources/list") == 0) {
        rc = handle_resources_list(server, &req.id);
    } else if (strcmp(req.method, "resources/read") == 0) {
        rc = handle_resources_read(server, &req.id, req.params);
    } else if (strcmp(req.method, "resources/subscribe") == 0) {
        rc = handle_resources_subscribe(server, &req.id, req.params);
    } else if (strcmp(req.method, "resources/unsubscribe") == 0) {
        rc = handle_resources_unsubscribe(server, &req.id, req.params);
    } else {
        /* Unknown method */
        if (!md_jsonrpc_is_notification(&req)) {
            rc = send_error(server, &req.id, MD_JSONRPC_METHOD_NOT_FOUND,
                            "Method not found");
        }
    }

    md_jsonrpc_request_free(&req);
    return rc;
}

/* ── Notifications ───────────────────────────────────────────── */

int md_mcp_server_notify_resource_updated(MdMcpServer *server,
                                          const char *uri)
{
    if (!server || !uri || !server->initialized)
        return -1;

    /* Check if subscribed */
    bool subscribed = false;
    for (int i = 0; i < server->subscription_count; i++) {
        if (strcmp(server->subscriptions[i], uri) == 0) {
            subscribed = true;
            break;
        }
    }
    if (!subscribed) return 0;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "uri", uri);

    char *json = md_jsonrpc_make_notification(
        "notifications/resources/updated", params);
    return send_json(server, json);
}

/* ── Lifecycle ───────────────────────────────────────────────── */

MdMcpServer *md_mcp_server_create(const MdMcpServerConfig *config)
{
    if (!config || !config->write_fn)
        return NULL;

    MdMcpServer *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->server_name = strdup(config->server_name ? config->server_name : "metadesk");
    s->server_version = strdup(config->server_version ? config->server_version : "0.1.0");
    s->write_fn = config->write_fn;
    s->write_userdata = config->write_userdata;
    pthread_mutex_init(&s->mu, NULL);

    return s;
}

void md_mcp_server_destroy(MdMcpServer *server)
{
    if (!server) return;

    /* Free tool input schemas */
    for (int i = 0; i < server->tool_count; i++) {
        if (server->tools[i].input_schema)
            cJSON_Delete(server->tools[i].input_schema);
    }

    /* Free subscriptions */
    for (int i = 0; i < server->subscription_count; i++)
        free(server->subscriptions[i]);

    pthread_mutex_destroy(&server->mu);
    free(server->server_name);
    free(server->server_version);
    free(server);
}

/* ── Registration ────────────────────────────────────────────── */

int md_mcp_server_register_tool(MdMcpServer *server, const MdMcpTool *tool)
{
    if (!server || !tool || !tool->name || !tool->handler)
        return -1;
    if (server->tool_count >= MD_MCP_MAX_TOOLS)
        return -1;

    MdMcpTool *slot = &server->tools[server->tool_count];
    slot->name = tool->name;
    slot->description = tool->description;
    slot->input_schema = tool->input_schema;  /* takes ownership */
    slot->handler = tool->handler;
    slot->userdata = tool->userdata;
    server->tool_count++;

    return 0;
}

int md_mcp_server_register_resource(MdMcpServer *server,
                                    const MdMcpResource *res)
{
    if (!server || !res || !res->uri || !res->read_handler)
        return -1;
    if (server->resource_count >= MD_MCP_MAX_RESOURCES)
        return -1;

    MdMcpResource *slot = &server->resources[server->resource_count];
    slot->uri = res->uri;
    slot->name = res->name;
    slot->description = res->description;
    slot->mime_type = res->mime_type;
    slot->read_handler = res->read_handler;
    slot->userdata = res->userdata;
    server->resource_count++;

    return 0;
}

/* ── State ───────────────────────────────────────────────────── */

bool md_mcp_server_is_initialized(const MdMcpServer *server)
{
    return server ? server->initialized : false;
}
