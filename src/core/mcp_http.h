/*
 * metadesk — mcp_http.h
 * HTTP + SSE transport for MCP server (Streamable HTTP).
 *
 * Provides a minimal HTTP/1.1 server that accepts:
 *   POST /mcp — JSON-RPC requests → JSON-RPC responses
 *   GET  /mcp — SSE stream for server→client notifications
 *
 * Default port: 7710 (next to stream port 7700).
 * Phase 2 feature — stdio transport is sufficient for Phase 1.
 */
#ifndef MD_MCP_HTTP_H
#define MD_MCP_HTTP_H

#include "mcp_server.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD_MCP_HTTP_DEFAULT_PORT 7710

typedef struct MdMcpHttp MdMcpHttp;

typedef struct {
    MdMcpServer  *server;        /* MCP server to dispatch to         */
    const char   *bind_addr;     /* NULL for localhost only            */
    uint16_t      port;          /* 0 for default (7710)              */
    int           max_clients;   /* max concurrent SSE clients (default: 4) */
} MdMcpHttpConfig;

/* Create an HTTP+SSE transport.
 * Does NOT start listening — call md_mcp_http_run(). */
MdMcpHttp *md_mcp_http_create(const MdMcpHttpConfig *config);

/* Run the HTTP server (blocking).
 * Returns 0 on clean shutdown, -1 on error. */
int md_mcp_http_run(MdMcpHttp *http);

/* Signal the server to shut down (thread-safe). */
void md_mcp_http_shutdown(MdMcpHttp *http);

/* Send an SSE event to all connected clients.
 * Used internally by the MCP server's write callback. */
int md_mcp_http_send_sse(MdMcpHttp *http, const char *event,
                         const char *data, size_t data_len);

/* Get the write function + userdata for MdMcpServerConfig.
 * The write function sends responses to the appropriate client. */
MdMcpWriteFn md_mcp_http_get_write_fn(MdMcpHttp *http);
void *md_mcp_http_get_write_userdata(MdMcpHttp *http);

/* Destroy the HTTP transport. */
void md_mcp_http_destroy(MdMcpHttp *http);

#ifdef __cplusplus
}
#endif

#endif /* MD_MCP_HTTP_H */
