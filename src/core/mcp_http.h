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
#define MD_MCP_SSE_WRITE_TIMEOUT_MS 2000

typedef struct MdMcpHttp MdMcpHttp;

typedef struct {
    MdMcpServer  *server;        /* MCP server to dispatch to         */
    const char   *bind_addr;     /* NULL for localhost only            */
    uint16_t      port;          /* 0 for default (7710)              */
    /* Auth configuration is copied at create time; files are read once. */
    const char   *token_file;    /* regular file, mode exactly 0600 */
    const char *const *public_urls; /* exact signed URLs ending in /mcp */
    size_t        public_url_count;
    const char *const *allow_npubs;
    size_t        allow_npub_count;
    const char   *allow_file;    /* one npub per line; blank/# lines ignored */
    /* Optional live NIP-98 grant source (md-ah9.3), e.g. the host role's
     * NIP-51 metadesk-allowlist. Returns the signer's MD_CAP_* grant
     * (session.h); only a grant including MD_CAP_AGENT is admitted, and
     * that grant gates the request's tools (md-5km: actions need
     * MD_CAP_INPUT too). Consulted only after the event's signature, kind,
     * time window, u, method and payload validate, and only for a signer
     * that the static allow_npubs/allow_file set does not contain (sources
     * are a union; static, bearer and credential-less loopback callers get
     * MD_MCP_CALLER_ALL_CAPS). Called again for every request, so a grant
     * change applies to the next one. Must be thread-safe, non-blocking and
     * fail closed: 0 while its source is not ready. Rechecked before every
     * SSE broadcast so a revoked stream is dropped without a restart. */
    uint32_t    (*nip98_grant)(const char *pubkey_hex, void *userdata);
    void         *nip98_grant_userdata;
    int           max_clients;   /* max concurrent SSE clients (default: 4) */
    int           sse_write_timeout_ms; /* response/whole-broadcast budget; <=0: 2000ms */
    int           sse_sndbuf;     /* test/diagnostic SO_SNDBUF limit for SSE only;
                                  * positive: bound per-client buffering,
                                  * 0: leave the kernel default unchanged */
    bool          ephemeral_port; /* test/diagnostic: bind :0, ignoring port;
                                   * false preserves port/default semantics */
} MdMcpHttpConfig;

/* Create an HTTP+SSE transport and bind/listen on its socket.
 * Call md_mcp_http_run() to accept and dispatch connections. */
MdMcpHttp *md_mcp_http_create(const MdMcpHttpConfig *config);

/* Bound port, including the kernel-selected ephemeral port; 0 for NULL. */
uint16_t md_mcp_http_port(const MdMcpHttp *http);

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
/* Broadcast one JSON message to all connected SSE clients. Matches
 * MdMcpWriteFn; userdata is the MdMcpHttp*. Used as the server's
 * notification sink. */
int md_mcp_http_write(const char *json, size_t len, void *userdata);

/* Test/diagnostic support: snapshot the number of ready SSE clients under
 * the client-list lock. Returns 0 for NULL. */
int md_mcp_http_sse_client_count(MdMcpHttp *http);

/* Test/diagnostic getsockopt(SO_SNDBUF) snapshot for a ready-client index.
 * Returns -1 for an invalid index or socket-option error. */
int md_mcp_http_sse_client_sndbuf(MdMcpHttp *http, int index);

/* Destroy the HTTP transport. */
void md_mcp_http_destroy(MdMcpHttp *http);

#ifdef __cplusplus
}
#endif

#endif /* MD_MCP_HTTP_H */
