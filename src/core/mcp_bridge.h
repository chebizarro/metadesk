/*
 * metadesk — mcp_bridge.h
 * MCP ↔ MdSession lifecycle bridge.
 *
 * Ties MCP server initialization to MdSession creation, and wires up
 * all the MCP tools, resources, and a11y change notifications.
 * This is the main integration point for hosting an MCP agent session.
 */
#ifndef MD_MCP_BRIDGE_H
#define MD_MCP_BRIDGE_H

#include "mcp_server.h"
#include "mcp_tools.h"
#include "mcp_resources.h"
#include "mcp_stdio.h"
#include "agent.h"
#include "a11y.h"
#include "input.h"
#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque bridge context */
typedef struct MdMcpBridge MdMcpBridge;

/* Configuration for creating an MCP bridge */
typedef struct {
    MdA11yCtx    *a11y;           /* shared a11y context (required)     */
    MdInput      *input;          /* shared input context (optional)    */
    MdTreeFormat  tree_format;    /* tree format for this session       */
    uint32_t      settle_ms;      /* settle time after actions (0=default) */

    /* Transport: use one of these */
    int           stdio_in_fd;    /* stdio transport: read fd (-1 to skip) */
    int           stdio_out_fd;   /* stdio transport: write fd             */
} MdMcpBridgeConfig;

/* Create the full MCP bridge: MdSession + MdAgent + MdMcpServer +
 * tools + resources + a11y change subscription.
 *
 * This is the one-call setup for hosting an MCP agent connection.
 * Returns NULL on failure. */
MdMcpBridge *md_mcp_bridge_create(const MdMcpBridgeConfig *config);

/* Run the MCP bridge event loop (blocking).
 * Returns when the transport closes (EOF) or an error occurs.
 * Returns 0 on clean shutdown, -1 on error. */
int md_mcp_bridge_run(MdMcpBridge *bridge);

/* Signal the bridge to shut down (thread-safe). */
void md_mcp_bridge_shutdown(MdMcpBridge *bridge);

/* Get the session state. */
MdSessionState md_mcp_bridge_get_state(const MdMcpBridge *bridge);

/* Get the underlying MCP server (for sending notifications). */
MdMcpServer *md_mcp_bridge_get_server(const MdMcpBridge *bridge);

/* Destroy the bridge and all owned resources. */
void md_mcp_bridge_destroy(MdMcpBridge *bridge);

#ifdef __cplusplus
}
#endif

#endif /* MD_MCP_BRIDGE_H */
