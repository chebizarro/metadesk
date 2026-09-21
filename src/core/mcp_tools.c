/*
 * metadesk — mcp_tools.c
 * MCP tool definitions and handlers for the 9 metadesk action types.
 *
 * One FieldDef table per tool drives BOTH the JSON Schema handed to
 * the LLM and the argument extraction/validation at call time — one
 * source of truth, so the two cannot disagree.
 */
#include "mcp_tools.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Field table ─────────────────────────────────────────────── */

typedef enum {
    FIELD_STRING,       /* fixed-size char[] in MdAction        */
    FIELD_STRING_ARRAY, /* keys[]                               */
    FIELD_INT,          /* dx / dy                             */
    FIELD_INT_ARRAY4,   /* region[4] — exactly 4 items required */
} FieldKind;

typedef struct {
    const char *name;
    FieldKind   kind;
    bool        required;
    const char *desc;
    size_t      offset;    /* offset of the char[] in MdAction (FIELD_STRING) */
    size_t      max_len;   /* sizeof of that char[]                    */
} FieldDef;

#define STR_FIELD(nm, req, dsc, fld) \
    { nm, FIELD_STRING, req, dsc, offsetof(MdAction, fld), sizeof(((MdAction *)0)->fld) }

static const FieldDef fields_target_only[] = {
    STR_FIELD("target_id", true, "Accessibility node ID to act on", target_id),
};
static const FieldDef fields_type_text[] = {
    STR_FIELD("target_id", true, "Accessibility node ID", target_id),
    STR_FIELD("text",      true, "Text to type",            text),
};
static const FieldDef fields_key_combo[] = {
    { "keys", FIELD_STRING_ARRAY, true, "Key names, e.g. [\"ctrl\", \"s\"]", 0, 0 },
};
static const FieldDef fields_scroll[] = {
    STR_FIELD("target_id", true,  "Accessibility node ID", target_id),
    { "dx",   FIELD_INT,    false, "Horizontal scroll delta", 0, 0 },
    { "dy",   FIELD_INT,    false, "Vertical scroll delta",   0, 0 },
};
static const FieldDef fields_set_value[] = {
    STR_FIELD("target_id", true, "Accessibility node ID", target_id),
    STR_FIELD("text",      true, "Value to set",           text),
};
static const FieldDef fields_screenshot[] = {
    { "region", FIELD_INT_ARRAY4, false,
      "Capture region [x, y, w, h]. Omit for full screen.", 0, 0 },
};

/* ── Schema emission (driven by the same table) ──────────────── */

static cJSON *field_schema(const FieldDef *f)
{
    cJSON *p = cJSON_CreateObject();
    switch (f->kind) {
    case FIELD_STRING:
        cJSON_AddStringToObject(p, "type", "string");
        break;
    case FIELD_STRING_ARRAY: {
        cJSON_AddStringToObject(p, "type", "array");
        cJSON *items = cJSON_CreateObject();
        cJSON_AddStringToObject(items, "type", "string");
        cJSON_AddItemToObject(p, "items", items);
        break;
    }
    case FIELD_INT:
        cJSON_AddStringToObject(p, "type", "integer");
        break;
    case FIELD_INT_ARRAY4: {
        cJSON_AddStringToObject(p, "type", "array");
        cJSON *items = cJSON_CreateObject();
        cJSON_AddStringToObject(items, "type", "integer");
        cJSON_AddItemToObject(p, "items", items);
        cJSON_AddNumberToObject(p, "minItems", 4);
        cJSON_AddNumberToObject(p, "maxItems", 4);
        break;
    }
    }
    if (f->desc)
        cJSON_AddStringToObject(p, "description", f->desc);
    return p;
}

static cJSON *build_schema(const FieldDef *fields, int field_count)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");

    cJSON *props = cJSON_CreateObject();
    cJSON *req = cJSON_CreateArray();
    for (int i = 0; i < field_count; i++) {
        cJSON_AddItemToObject(props, fields[i].name,
                              field_schema(&fields[i]));
        if (fields[i].required)
            cJSON_AddItemToArray(req, cJSON_CreateString(fields[i].name));
    }
    cJSON_AddItemToObject(s, "properties", props);
    if (cJSON_GetArraySize(req) > 0)
        cJSON_AddItemToObject(s, "required", req);
    else
        cJSON_Delete(req);
    cJSON_AddFalseToObject(s, "additionalProperties");
    return s;
}

/* ── Argument extraction + validation (driven by the same table) ── */

/* Extract arguments into `action` per the table.
 * Returns 0 on success; on failure sets *error_msg (malloc'd) and
 * returns -1. Over-length strings and wrong-shaped regions are
 * errors, never silent truncation. */
static int extract_args(const FieldDef *fields, int field_count,
                        const cJSON *arguments, MdAction *action,
                        char **error_msg)
{
    if (arguments && !cJSON_IsObject(arguments)) {
        if (asprintf(error_msg, "Arguments must be an object") < 0)
            *error_msg = NULL;
        return -1;
    }

    for (int i = 0; i < field_count; i++) {
        const FieldDef *f = &fields[i];
        const cJSON *item =
            arguments ? cJSON_GetObjectItemCaseSensitive(arguments, f->name)
                      : NULL;

        if (!item) {
            if (f->required) {
                if (asprintf(error_msg, "Missing required '%s'", f->name) < 0)
                    *error_msg = NULL;
                return -1;
            }
            continue;
        }

        switch (f->kind) {
        case FIELD_STRING: {
            if (!cJSON_IsString(item)) {
                if (asprintf(error_msg, "'%s' must be a string", f->name) < 0)
                    *error_msg = NULL;
                return -1;
            }
            size_t len = strlen(item->valuestring);
            if (len >= f->max_len) {
                if (asprintf(error_msg,
                             "'%s' is %zu chars; maximum is %zu",
                             f->name, len, f->max_len - 1) < 0)
                    *error_msg = NULL;
                return -1;
            }
            char *dst = (char *)action + f->offset;
            memcpy(dst, item->valuestring, len);
            dst[len] = '\0';
            break;
        }
        case FIELD_STRING_ARRAY: {
            if (!cJSON_IsArray(item)) {
                if (asprintf(error_msg, "'%s' must be an array", f->name) < 0)
                    *error_msg = NULL;
                return -1;
            }
            int count = cJSON_GetArraySize(item);
            if (count == 0) {
                if (asprintf(error_msg, "'%s' must not be empty", f->name) < 0)
                    *error_msg = NULL;
                return -1;
            }
            if (count > MD_MAX_KEYS)
                count = MD_MAX_KEYS;
            for (int k = 0; k < count; k++) {
                const cJSON *el = cJSON_GetArrayItem(item, k);
                if (!cJSON_IsString(el)) {
                    if (asprintf(error_msg,
                                 "'%s' entries must be strings", f->name) < 0)
                        *error_msg = NULL;
                    return -1;
                }
                action->keys[action->key_count++] = strdup(el->valuestring);
            }
            break;
        }
        case FIELD_INT: {
            if (!cJSON_IsNumber(item)) {
                if (asprintf(error_msg, "'%s' must be an integer", f->name) < 0)
                    *error_msg = NULL;
                return -1;
            }
            if (strcmp(f->name, "dx") == 0)
                action->dx = item->valueint;
            else
                action->dy = item->valueint;
            break;
        }
        case FIELD_INT_ARRAY4: {
            if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 4) {
                if (asprintf(error_msg,
                             "'%s' must be an array of exactly 4 integers [x, y, w, h]",
                             f->name) < 0)
                    *error_msg = NULL;
                return -1;
            }
            for (int k = 0; k < 4; k++) {
                const cJSON *el = cJSON_GetArrayItem(item, k);
                if (!cJSON_IsNumber(el)) {
                    if (asprintf(error_msg,
                                 "'%s' entries must be integers", f->name) < 0)
                        *error_msg = NULL;
                    return -1;
                }
                action->region[k] = el->valueint;
            }
            break;
        }
        }
    }
    return 0;
}

/* ── Tool handler ────────────────────────────────────────────── */

typedef struct {
    MdMcpToolCtx  *ctx;
    MdActionType   action_type;
    const FieldDef *fields;
    int            field_count;
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

    MdAction action;
    memset(&action, 0, sizeof(action));
    action.type = hctx->action_type;

    if (extract_args(hctx->fields, hctx->field_count,
                     arguments, &action, error_msg) != 0) {
        md_action_cleanup(&action);
        *is_error = true;
        return make_text_content(*error_msg ? *error_msg : "Invalid arguments");
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

    cJSON *result = make_text_content(result_str);
    free(result_str);
    return result;
}

/* ── Tool definitions ────────────────────────────────────────── */

typedef struct {
    const char    *name;
    const char    *description;
    MdActionType   action_type;
    const FieldDef *fields;
    int            field_count;
} ToolDef;

#define TOOL(nm, dsc, type, flds) \
    { nm, dsc, type, flds, (int)(sizeof(flds) / sizeof((flds)[0])) }

static const ToolDef tool_defs[] = {
    TOOL("metadesk_click",       "Click a UI element by its accessibility node ID",
         MD_ACTION_CLICK,       fields_target_only),
    TOOL("metadesk_dbl_click",   "Double-click a UI element",
         MD_ACTION_DBL_CLICK,   fields_target_only),
    TOOL("metadesk_right_click", "Right-click a UI element",
         MD_ACTION_RIGHT_CLICK, fields_target_only),
    TOOL("metadesk_type",        "Type text into a UI element",
         MD_ACTION_TYPE,        fields_type_text),
    TOOL("metadesk_key_combo",   "Press a keyboard combination (e.g. ctrl+s)",
         MD_ACTION_KEY_COMBO,   fields_key_combo),
    TOOL("metadesk_scroll",      "Scroll at a UI element",
         MD_ACTION_SCROLL,      fields_scroll),
    TOOL("metadesk_focus",       "Move focus to a UI element",
         MD_ACTION_FOCUS,       fields_target_only),
    TOOL("metadesk_set_value",   "Set the value of a UI element directly",
         MD_ACTION_SET_VALUE,   fields_set_value),
    TOOL("metadesk_screenshot",  "Capture a screenshot (region or full screen)",
         MD_ACTION_SCREENSHOT,  fields_screenshot),
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
        hctxs[i].fields = tool_defs[i].fields;
        hctxs[i].field_count = tool_defs[i].field_count;

        MdMcpTool tool = {
            .name = tool_defs[i].name,
            .description = tool_defs[i].description,
            .input_schema = build_schema(tool_defs[i].fields,
                                         tool_defs[i].field_count),
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
