/*
 * metadesk — mcp_tools.h
 * MCP tool definitions for the 9 metadesk action types.
 *
 * Registers tools with an MdMcpServer so agents discover them via tools/list.
 * Tool handlers bridge MCP calls into the MdAgent action pipeline.
 */
#ifndef MD_MCP_TOOLS_H
#define MD_MCP_TOOLS_H

#include "mcp_server.h"
#include "agent.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Context passed to all MCP tool handlers — holds references to the
 * agent and a11y subsystems needed to execute actions. */
typedef struct {
    MdAgent   *agent;   /* agent handler for action dispatch     */
    MdA11yCtx *a11y;    /* shared a11y context (for tree reads)  */
} MdMcpToolCtx;

/* Register all 9 metadesk action tools with the MCP server.
 * The tool_ctx must remain valid for the lifetime of the server.
 * Returns 0 on success, -1 if any registration fails. */
int md_mcp_register_tools(MdMcpServer *server, MdMcpToolCtx *tool_ctx);

#ifdef __cplusplus
}
#endif

#endif /* MD_MCP_TOOLS_H */
