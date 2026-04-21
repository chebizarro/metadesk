/*
 * metadesk — mcp_bridge.c
 * MCP ↔ MdSession lifecycle bridge implementation.
 *
 * Creates and owns: MdSession, MdAgent, MdMcpServer, MdMcpStdio.
 * Registers tools, resources, and a11y change notifications.
 */
#include "mcp_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <uuid/uuid.h>

struct MdMcpBridge {
    MdSession          session;
    MdAgent           *agent;
    MdMcpServer       *server;
    MdMcpStdio        *stdio_ctx;

    /* Contexts for tool/resource handlers (must outlive server) */
    MdMcpToolCtx       tool_ctx;
    MdMcpResourceCtx   resource_ctx;

    /* Borrowed references */
    MdA11yCtx         *a11y;
    MdInput           *input;
};

/* ── Write callback for stdio mode ───────────────────────────── */

static int bridge_stdio_write(const char *json, size_t len, void *userdata)
{
    MdMcpBridge *b = (MdMcpBridge *)userdata;
    if (!b || !b->stdio_ctx) return -1;

    MdMcpWriteFn fn = md_mcp_stdio_get_write_fn(b->stdio_ctx);
    void *ud = md_mcp_stdio_get_write_userdata(b->stdio_ctx);
    return fn(json, len, ud);
}

/* ── Lifecycle ───────────────────────────────────────────────── */

MdMcpBridge *md_mcp_bridge_create(const MdMcpBridgeConfig *config)
{
    if (!config) return NULL;

    MdMcpBridge *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->a11y = config->a11y;
    b->input = config->input;

    /* 1. Initialize session */
    md_session_init(&b->session);
    b->session.capabilities = MD_CAP_AGENT;
    if (config->input) b->session.capabilities |= MD_CAP_INPUT;
    b->session.state = MD_SESSION_NEGOTIATING;

    /* Generate session ID */
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, b->session.session_id);

    /* 2. Create agent */
    MdAgentConfig agent_cfg = {
        .a11y = config->a11y,
        .input = config->input,
        .tree_format = config->tree_format,
        .settle_ms = config->settle_ms > 0 ? config->settle_ms
                                            : MD_AGENT_DEFAULT_SETTLE_MS,
    };
    b->agent = md_agent_create(&agent_cfg);
    if (!b->agent) {
        free(b);
        return NULL;
    }

    /* 3. Create MCP server */
    MdMcpServerConfig srv_cfg = {
        .server_name = "metadesk",
        .server_version = "0.1.0",
        .write_fn = bridge_stdio_write,
        .write_userdata = b,
    };
    b->server = md_mcp_server_create(&srv_cfg);
    if (!b->server) {
        md_agent_destroy(b->agent);
        free(b);
        return NULL;
    }

    /* 4. Register tools */
    b->tool_ctx.agent = b->agent;
    b->tool_ctx.a11y = config->a11y;
    md_mcp_register_tools(b->server, &b->tool_ctx);

    /* 5. Register resources */
    b->resource_ctx.a11y = config->a11y;
    b->resource_ctx.session = &b->session;
    b->resource_ctx.agent = b->agent;
    b->resource_ctx.tree_format = config->tree_format;
    md_mcp_register_resources(b->server, &b->resource_ctx);

    /* 6. Subscribe to a11y changes for notifications */
    if (config->a11y) {
        md_a11y_subscribe_changes(config->a11y,
                                  md_mcp_a11y_change_cb,
                                  b->server);
    }

    /* 7. Create stdio transport if requested */
    if (config->stdio_in_fd >= 0) {
        b->stdio_ctx = md_mcp_stdio_create(b->server,
                                            config->stdio_in_fd,
                                            config->stdio_out_fd);
        if (!b->stdio_ctx) {
            md_mcp_server_destroy(b->server);
            md_agent_destroy(b->agent);
            free(b);
            return NULL;
        }
    }

    /* 8. Activate session */
    md_session_activate(&b->session);

    return b;
}

int md_mcp_bridge_run(MdMcpBridge *bridge)
{
    if (!bridge) return -1;

    if (bridge->stdio_ctx) {
        return md_mcp_stdio_run(bridge->stdio_ctx);
    }

    /* No transport configured */
    return -1;
}

void md_mcp_bridge_shutdown(MdMcpBridge *bridge)
{
    if (!bridge) return;
    if (bridge->stdio_ctx)
        md_mcp_stdio_shutdown(bridge->stdio_ctx);
}

MdSessionState md_mcp_bridge_get_state(const MdMcpBridge *bridge)
{
    return bridge ? bridge->session.state : MD_SESSION_IDLE;
}

MdMcpServer *md_mcp_bridge_get_server(const MdMcpBridge *bridge)
{
    return bridge ? bridge->server : NULL;
}

void md_mcp_bridge_destroy(MdMcpBridge *bridge)
{
    if (!bridge) return;

    md_session_disconnect(&bridge->session);

    if (bridge->stdio_ctx)
        md_mcp_stdio_destroy(bridge->stdio_ctx);
    if (bridge->server)
        md_mcp_server_destroy(bridge->server);
    if (bridge->agent)
        md_agent_destroy(bridge->agent);

    free(bridge);
}
