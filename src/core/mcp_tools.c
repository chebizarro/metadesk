/*
 * metadesk — mcp_tools.c
 * MCP tool definitions and handlers for the 9 metadesk action types.
 *
 * Each tool handler converts MCP arguments → MdAction → agent pipeline,
 * and returns the resulting UI tree delta (or error) as MCP content.
 */
#include "mcp_tools.h"
#include "action.h"
#include "a11y.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Schema builders ─────────────────────────────────────────── */

/* Build a JSON Schema object with "type":"object" and given properties. */
static cJSON *schema_object(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    return s;
}

static void schema_add_string_prop(cJSON *props, const char *name,
                                   const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "string");
    if (desc) cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static void schema_add_integer_prop(cJSON *props, const char *name,
                                    const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "integer");
    if (desc) cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static void schema_add_string_array_prop(cJSON *props, const char *name,
                                         const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "array");
    cJSON *items = cJSON_CreateObject();
    cJSON_AddStringToObject(items, "type", "string");
    cJSON_AddItemToObject(p, "items", items);
    if (desc) cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static void schema_add_int_array_prop(cJSON *props, const char *name,
                                      const char *desc, int min_items,
                                      int max_items)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "array");
    cJSON *items = cJSON_CreateObject();
    cJSON_AddStringToObject(items, "type", "integer");
    cJSON_AddItemToObject(p, "items", items);
    if (min_items >= 0) cJSON_AddNumberToObject(p, "minItems", min_items);
    if (max_items >= 0) cJSON_AddNumberToObject(p, "maxItems", max_items);
    if (desc) cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

/* Build schema for target-only tools (click, dbl_click, right_click, focus) */
static cJSON *schema_target_only(void)
{
    cJSON *s = schema_object();
    cJSON *props = cJSON_CreateObject();
    schema_add_string_prop(props, "target_id", "Accessibility node ID to act on");
    cJSON_AddItemToObject(s, "properties", props);
    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("target_id"));
    cJSON_AddItemToObject(s, "required", req);
    cJSON_AddFalseToObject(s, "additionalProperties");
    return s;
}

/* ── Generic tool handler ────────────────────────────────────── */

/* All 9 tools funnel through this handler. The action type is encoded
 * in the userdata via offset from the MdMcpToolCtx pointer. We use a
 * small wrapper struct instead. */

typedef struct {
    MdMcpToolCtx  *ctx;
    MdActionType   action_type;
} ToolHandlerCtx;

static cJSON *make_text_content(const char *text)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(arr, item);
    return arr;
}

static cJSON *tool_handler(const cJSON *arguments,
                           bool *is_error, char **error_msg,
                           void *userdata)
{
    ToolHandlerCtx *hctx = (ToolHandlerCtx *)userdata;
    MdMcpToolCtx *ctx = hctx->ctx;
    MdActionType action_type = hctx->action_type;

    /* Build MdAction from MCP arguments */
    MdAction action;
    memset(&action, 0, sizeof(action));
    action.type = action_type;

    /* Extract target_id if present */
    if (arguments) {
        cJSON *tid = cJSON_GetObjectItemCaseSensitive(arguments, "target_id");
        if (cJSON_IsString(tid)) {
            strncpy(action.target_id, tid->valuestring,
                    sizeof(action.target_id) - 1);
        }
    }

    /* Validate: most actions require target_id */
    if (action_type != MD_ACTION_KEY_COMBO &&
        action_type != MD_ACTION_SCREENSHOT &&
        action.target_id[0] == '\0') {
        *is_error = true;
        *error_msg = strdup("Missing required 'target_id'");
        return make_text_content("Error: missing target_id");
    }

    /* Extract type-specific fields */
    if (arguments) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(arguments, "text");
        if (cJSON_IsString(text)) {
            strncpy(action.text, text->valuestring,
                    sizeof(action.text) - 1);
        }

        cJSON *keys = cJSON_GetObjectItemCaseSensitive(arguments, "keys");
        if (cJSON_IsArray(keys)) {
            int count = cJSON_GetArraySize(keys);
            if (count > MD_MAX_KEYS) count = MD_MAX_KEYS;
            for (int i = 0; i < count; i++) {
                cJSON *k = cJSON_GetArrayItem(keys, i);
                if (cJSON_IsString(k))
                    action.keys[action.key_count++] = strdup(k->valuestring);
            }
        }

        cJSON *dx = cJSON_GetObjectItemCaseSensitive(arguments, "dx");
        if (cJSON_IsNumber(dx)) action.dx = dx->valueint;

        cJSON *dy = cJSON_GetObjectItemCaseSensitive(arguments, "dy");
        if (cJSON_IsNumber(dy)) action.dy = dy->valueint;

        cJSON *region = cJSON_GetObjectItemCaseSensitive(arguments, "region");
        if (cJSON_IsArray(region) && cJSON_GetArraySize(region) == 4) {
            for (int i = 0; i < 4; i++) {
                cJSON *el = cJSON_GetArrayItem(region, i);
                if (cJSON_IsNumber(el))
                    action.region[i] = el->valueint;
            }
        }
    }

    /* Validate type-specific requirements */
    if (action_type == MD_ACTION_TYPE && action.text[0] == '\0') {
        *is_error = true;
        *error_msg = strdup("Missing required 'text' for type action");
        md_action_cleanup(&action);
        return make_text_content("Error: missing text");
    }
    if (action_type == MD_ACTION_KEY_COMBO && action.key_count == 0) {
        *is_error = true;
        *error_msg = strdup("Missing required 'keys' for key_combo action");
        md_action_cleanup(&action);
        return make_text_content("Error: missing keys");
    }
    if (action_type == MD_ACTION_SET_VALUE && action.text[0] == '\0') {
        *is_error = true;
        *error_msg = strdup("Missing required 'text' for set_value action");
        md_action_cleanup(&action);
        return make_text_content("Error: missing text");
    }

    /* Encode action to JSON for the agent pipeline */
    char *action_json = md_action_encode(&action);
    md_action_cleanup(&action);

    if (!action_json) {
        *is_error = true;
        *error_msg = strdup("Failed to encode action");
        return make_text_content("Error: action encode failed");
    }

    if (!ctx->agent) {
        *is_error = true;
        *error_msg = strdup("No agent session active");
        free(action_json);
        return make_text_content("Error: no agent session active — "
                                 "the MCP bridge was created without an agent");
    }

    /* Full path: dispatch through agent pipeline and return delta */
    char *result_str = md_agent_handle_action_mcp(
        ctx->agent,
        (const uint8_t *)action_json, (uint32_t)strlen(action_json));
    free(action_json);

    if (!result_str) {
        *is_error = true;
        *error_msg = strdup("Agent action execution failed");
        return make_text_content("Error: action execution failed");
    }

    cJSON *arr = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", result_str);
    cJSON_AddItemToArray(arr, item);
    free(result_str);
    return arr;
}

/* ── Tool definitions ────────────────────────────────────────── */

typedef struct {
    const char   *name;
    const char   *description;
    MdActionType  action_type;
    cJSON      *(*make_schema)(void);
} ToolDef;

static cJSON *schema_type_text(void)
{
    cJSON *s = schema_object();
    cJSON *props = cJSON_CreateObject();
    schema_add_string_prop(props, "target_id", "Accessibility node ID");
    schema_add_string_prop(props, "text", "Text to type");
    cJSON_AddItemToObject(s, "properties", props);
    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("target_id"));
    cJSON_AddItemToArray(req, cJSON_CreateString("text"));
    cJSON_AddItemToObject(s, "required", req);
    cJSON_AddFalseToObject(s, "additionalProperties");
    return s;
}

static cJSON *schema_key_combo(void)
{
    cJSON *s = schema_object();
    cJSON *props = cJSON_CreateObject();
    schema_add_string_array_prop(props, "keys",
                                 "Key names, e.g. [\"ctrl\", \"s\"]");
    cJSON_AddItemToObject(s, "properties", props);
    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("keys"));
    cJSON_AddItemToObject(s, "required", req);
    cJSON_AddFalseToObject(s, "additionalProperties");
    return s;
}

static cJSON *schema_scroll(void)
{
    cJSON *s = schema_object();
    cJSON *props = cJSON_CreateObject();
    schema_add_string_prop(props, "target_id", "Accessibility node ID");
    schema_add_integer_prop(props, "dx", "Horizontal scroll delta");
    schema_add_integer_prop(props, "dy", "Vertical scroll delta");
    cJSON_AddItemToObject(s, "properties", props);
    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("target_id"));
    cJSON_AddItemToObject(s, "required", req);
    cJSON_AddFalseToObject(s, "additionalProperties");
    return s;
}

static cJSON *schema_set_value(void)
{
    cJSON *s = schema_object();
    cJSON *props = cJSON_CreateObject();
    schema_add_string_prop(props, "target_id", "Accessibility node ID");
    schema_add_string_prop(props, "text", "Value to set");
    cJSON_AddItemToObject(s, "properties", props);
    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("target_id"));
    cJSON_AddItemToArray(req, cJSON_CreateString("text"));
    cJSON_AddItemToObject(s, "required", req);
    cJSON_AddFalseToObject(s, "additionalProperties");
    return s;
}

static cJSON *schema_screenshot(void)
{
    cJSON *s = schema_object();
    cJSON *props = cJSON_CreateObject();
    schema_add_int_array_prop(props, "region",
                              "Capture region [x, y, w, h]. Omit for full screen.",
                              4, 4);
    cJSON_AddItemToObject(s, "properties", props);
    /* No required fields — omitting region means full screen */
    cJSON_AddFalseToObject(s, "additionalProperties");
    return s;
}

static const ToolDef tool_defs[] = {
    { "metadesk_click",       "Click a UI element by its accessibility node ID",
      MD_ACTION_CLICK,       schema_target_only },
    { "metadesk_dbl_click",   "Double-click a UI element",
      MD_ACTION_DBL_CLICK,   schema_target_only },
    { "metadesk_right_click", "Right-click a UI element",
      MD_ACTION_RIGHT_CLICK, schema_target_only },
    { "metadesk_type",        "Type text into a UI element",
      MD_ACTION_TYPE,        schema_type_text },
    { "metadesk_key_combo",   "Press a keyboard combination (e.g. ctrl+s)",
      MD_ACTION_KEY_COMBO,   schema_key_combo },
    { "metadesk_scroll",      "Scroll at a UI element",
      MD_ACTION_SCROLL,      schema_scroll },
    { "metadesk_focus",       "Move focus to a UI element",
      MD_ACTION_FOCUS,       schema_target_only },
    { "metadesk_set_value",   "Set the value of a UI element directly",
      MD_ACTION_SET_VALUE,   schema_set_value },
    { "metadesk_screenshot",  "Capture a screenshot (region or full screen)",
      MD_ACTION_SCREENSHOT,  schema_screenshot },
};

#define TOOL_DEF_COUNT (sizeof(tool_defs) / sizeof(tool_defs[0]))

/* ── Registration ────────────────────────────────────────────── */

int md_mcp_register_tools(MdMcpServer *server, MdMcpToolCtx *tool_ctx)
{
    if (!server || !tool_ctx) return -1;

    /* Allocate handler contexts on the heap — one per tool.
     * These are owned by the caller via tool_ctx and must outlive the server. */
    ToolHandlerCtx *hctxs = calloc(TOOL_DEF_COUNT, sizeof(ToolHandlerCtx));
    if (!hctxs) return -1;
    tool_ctx->_handler_ctxs = hctxs;

    for (size_t i = 0; i < TOOL_DEF_COUNT; i++) {
        hctxs[i].ctx = tool_ctx;
        hctxs[i].action_type = tool_defs[i].action_type;

        MdMcpTool tool = {
            .name = tool_defs[i].name,
            .description = tool_defs[i].description,
            .input_schema = tool_defs[i].make_schema(),
            .handler = tool_handler,
            .userdata = &hctxs[i],
        };

        if (!tool.input_schema || md_mcp_server_register_tool(server, &tool) != 0) {
            if (tool.input_schema)
                cJSON_Delete(tool.input_schema);
            for (size_t j = 0; j < i; j++)
                md_mcp_server_unregister_tool(server, tool_defs[j].name);
            free(hctxs);
            tool_ctx->_handler_ctxs = NULL;
            return -1;
        }
    }

    return 0;
}

void md_mcp_tools_cleanup(MdMcpToolCtx *tool_ctx)
{
    if (!tool_ctx) return;
    free(tool_ctx->_handler_ctxs);
    tool_ctx->_handler_ctxs = NULL;
}
