/*
 * metadesk — mcp_resources.c
 * MCP resource implementations: UI tree and session info.
 */
#include "mcp_resources.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── UI tree resource ────────────────────────────────────────── */

static cJSON *read_ui_tree(void *userdata)
{
    MdMcpResourceCtx *ctx = (MdMcpResourceCtx *)userdata;

    cJSON *contents = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "uri", "metadesk://ui-tree");

    /* Walk the accessibility tree */
    char *tree_text = NULL;
    const char *mime = "application/json";

    if (ctx->a11y) {
        MdA11yNode *root = md_a11y_walk(ctx->a11y);
        if (root) {
            switch (ctx->tree_format) {
            case MD_TREE_FORMAT_COMPACT:
                tree_text = md_a11y_to_compact(root);
                mime = "text/plain";
                break;
            case MD_TREE_FORMAT_JSON:
            default:
                tree_text = md_a11y_to_json(root);
                mime = "application/json";
                break;
            }
            md_a11y_node_free(root);
        }
    }

    cJSON_AddStringToObject(item, "mimeType", mime);
    cJSON_AddStringToObject(item, "text",
                            tree_text ? tree_text : "{\"root\":null}");
    free(tree_text);

    cJSON_AddItemToArray(contents, item);
    return contents;
}

/* ── Session info resource ───────────────────────────────────── */

static cJSON *read_session_info(void *userdata)
{
    MdMcpResourceCtx *ctx = (MdMcpResourceCtx *)userdata;

    cJSON *info = cJSON_CreateObject();

    if (ctx->session) {
        if (ctx->session->session_id[0])
            cJSON_AddStringToObject(info, "session_id", ctx->session->session_id);
        if (ctx->session->peer_npub[0])
            cJSON_AddStringToObject(info, "peer_npub", ctx->session->peer_npub);

        /* State */
        const char *state_str = "unknown";
        switch (ctx->session->state) {
        case MD_SESSION_IDLE:          state_str = "idle"; break;
        case MD_SESSION_REQUESTING:    state_str = "requesting"; break;
        case MD_SESSION_NEGOTIATING:   state_str = "negotiating"; break;
        case MD_SESSION_ACTIVE:        state_str = "active"; break;
        case MD_SESSION_DISCONNECTING: state_str = "disconnecting"; break;
        }
        cJSON_AddStringToObject(info, "state", state_str);

        /* Capabilities */
        cJSON *caps = cJSON_CreateArray();
        if (ctx->session->capabilities & MD_CAP_VIDEO)
            cJSON_AddItemToArray(caps, cJSON_CreateString("video"));
        if (ctx->session->capabilities & MD_CAP_AGENT)
            cJSON_AddItemToArray(caps, cJSON_CreateString("agent"));
        if (ctx->session->capabilities & MD_CAP_INPUT)
            cJSON_AddItemToArray(caps, cJSON_CreateString("input"));
        cJSON_AddItemToObject(info, "capabilities", caps);

        cJSON_AddStringToObject(info, "tree_format",
                                ctx->tree_format == MD_TREE_FORMAT_COMPACT
                                    ? "compact" : "json");
        cJSON_AddNumberToObject(info, "keepalive_ms", ctx->session->keepalive_ms);
    }

    if (ctx->agent) {
        cJSON_AddNumberToObject(info, "action_count",
                                md_agent_get_action_count(ctx->agent));
    }

    /* Serialize info to a string, then wrap in MCP content array */
    char *info_str = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);

    cJSON *contents = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "uri", "metadesk://session-info");
    cJSON_AddStringToObject(item, "mimeType", "application/json");
    cJSON_AddStringToObject(item, "text", info_str ? info_str : "{}");
    free(info_str);
    cJSON_AddItemToArray(contents, item);

    return contents;
}

/* ── Notifications ───────────────────────────────────────────── */

int md_mcp_notify_tree_changed(MdMcpServer *server)
{
    if (!server) return -1;
    return md_mcp_server_notify_resource_updated(server, "metadesk://ui-tree");
}

void md_mcp_a11y_change_cb(const MdA11yDelta *deltas, int count,
                           void *userdata)
{
    (void)deltas;
    (void)count;
    MdMcpServer *server = (MdMcpServer *)userdata;
    md_mcp_notify_tree_changed(server);
}

/* ── Registration ────────────────────────────────────────────── */

int md_mcp_register_resources(MdMcpServer *server, MdMcpResourceCtx *res_ctx)
{
    if (!server || !res_ctx) return -1;

    MdMcpResource ui_tree = {
        .uri = "metadesk://ui-tree",
        .name = "UI Tree",
        .description = "Current accessibility tree of the remote desktop. "
                       "Returns the full tree in the negotiated format "
                       "(JSON or compact).",
        .mime_type = "application/json",
        .read_handler = read_ui_tree,
        .userdata = res_ctx,
    };
    if (md_mcp_server_register_resource(server, &ui_tree) != 0)
        return -1;

    MdMcpResource session_info = {
        .uri = "metadesk://session-info",
        .name = "Session Info",
        .description = "Current session metadata: state, capabilities, "
                       "peer identity, and action count.",
        .mime_type = "application/json",
        .read_handler = read_session_info,
        .userdata = res_ctx,
    };
    if (md_mcp_server_register_resource(server, &session_info) != 0)
        return -1;

    return 0;
}
