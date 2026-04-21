/*
 * metadesk — mcp_stdio.h
 * stdio transport for MCP server.
 *
 * Reads newline-delimited JSON from an input fd, dispatches to the MCP
 * server, and writes responses as newline-delimited JSON to an output fd.
 */
#ifndef MD_MCP_STDIO_H
#define MD_MCP_STDIO_H

#include "mcp_server.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdMcpStdio MdMcpStdio;

/* Create a stdio transport wired to the given MCP server.
 * in_fd: file descriptor to read from (e.g. STDIN_FILENO)
 * out_fd: file descriptor to write to (e.g. STDOUT_FILENO) */
MdMcpStdio *md_mcp_stdio_create(MdMcpServer *server, int in_fd, int out_fd);

/* Run the blocking event loop. Returns when EOF is reached on in_fd
 * or md_mcp_stdio_shutdown() is called.
 * Returns 0 on clean shutdown, -1 on error. */
int md_mcp_stdio_run(MdMcpStdio *stdio_ctx);

/* Signal the run loop to exit (thread-safe). */
void md_mcp_stdio_shutdown(MdMcpStdio *stdio_ctx);

/* Destroy the stdio transport. */
void md_mcp_stdio_destroy(MdMcpStdio *stdio_ctx);

/* Get the write function + userdata for configuring MdMcpServerConfig.
 * This allows the server to write responses via this transport. */
MdMcpWriteFn md_mcp_stdio_get_write_fn(MdMcpStdio *stdio_ctx);
void *md_mcp_stdio_get_write_userdata(MdMcpStdio *stdio_ctx);

#ifdef __cplusplus
}
#endif

#endif /* MD_MCP_STDIO_H */
