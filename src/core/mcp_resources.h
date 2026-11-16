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
#include "a11y_snapshot.h"
#include "session.h"
#include "agent.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical resource URIs */
#define MD_MCP_URI_UI_TREE     "metadesk://ui-tree"
#define MD_MCP_URI_SESSION_INFO "metadesk://session-info"

typedef struct MdMcpNotifyCoalescer MdMcpNotifyCoalescer;

/* Context passed to MCP resource handlers. */
typedef struct {
    MdA11ySnapshot *snapshot;  /* borrowed; shared MCP read snapshot */
    MdMcpServer  *server;      /* notification destination           */
    MdSession    *session;     /* current session state               */
    MdAgent      *agent;       /* agent handler (for action count)    */
    MdMcpNotifyCoalescer *notifier; /* optional; NULL = synchronous hints */
    MdTreeFormat  tree_format; /* negotiated tree format              */
} MdMcpResourceCtx;

/* Register all metadesk resources with the MCP server.
 * The res_ctx must remain valid for the lifetime of the server.
 * Returns 0 on success, -1 if any registration fails. */
int md_mcp_register_resources(MdMcpServer *server, MdMcpResourceCtx *res_ctx);

/* Notify the MCP server that the UI tree has changed.
 * Only sends a notification if the client has subscribed to metadesk://ui-tree.
 * Call this after an accessibility change hint.
 * Returns 0 on success, -1 on error. */
int md_mcp_notify_tree_changed(MdMcpServer *server);

/* Start after server creation, stop after unsubscribe/drain and before transports.
 * create returns NULL on failure; quiet_ms must be nonzero (capped at 200). */
MdMcpNotifyCoalescer *md_mcp_notify_coalescer_create(MdMcpServer *server,
                                                  uint32_t quiet_ms);
void md_mcp_notify_coalescer_destroy(MdMcpNotifyCoalescer *notifier);

/* A11y change callback suitable for md_a11y_subscribe_changes().
 * Pass the MdMcpResourceCtx* as userdata (must outlive callbacks).
 * Invalidates its snapshot before notifying metadesk://ui-tree. */
void md_mcp_a11y_change_cb(const MdA11yDelta *deltas, int count,
                           void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* MD_MCP_RESOURCES_H */
