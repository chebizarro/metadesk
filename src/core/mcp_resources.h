/*
 * metadesk — mcp_resources.h
 * MCP resource definitions — UI tree and session info.
 *
 * Registers resources with an MdMcpServer so agents can read
 * the current UI state and session metadata via resources/read.
 */
#ifndef MD_MCP_RESOURCES_H
#define MD_MCP_RESOURCES_H

#include "mcp_server.h"
#include "a11y.h"
#include "session.h"
#include "agent.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Context passed to MCP resource handlers. */
typedef struct {
    MdA11yCtx    *a11y;        /* accessibility tree context         */
    MdSession    *session;     /* current session state               */
    MdAgent      *agent;       /* agent handler (for action count)    */
    MdTreeFormat  tree_format; /* negotiated tree format              */
} MdMcpResourceCtx;

/* Register all metadesk resources with the MCP server.
 * The res_ctx must remain valid for the lifetime of the server.
 * Returns 0 on success, -1 if any registration fails. */
int md_mcp_register_resources(MdMcpServer *server, MdMcpResourceCtx *res_ctx);

/* Notify the MCP server that the UI tree has changed.
 * Only sends a notification if the client has subscribed to metadesk://ui-tree.
 * Call this after md_a11y_diff() detects changes.
 * Returns 0 on success, -1 on error. */
int md_mcp_notify_tree_changed(MdMcpServer *server);

/* A11y change callback suitable for md_a11y_subscribe_changes().
 * Pass the MdMcpServer* as userdata. Triggers resource-updated
 * notification for metadesk://ui-tree. */
void md_mcp_a11y_change_cb(const MdA11yDelta *deltas, int count,
                           void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* MD_MCP_RESOURCES_H */
