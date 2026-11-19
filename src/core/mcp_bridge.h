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
#include "mcp_http.h"
#include "agent.h"
#include "a11y.h"
#include "input.h"
#include "session.h"
#include <stdbool.h>

/* Version string — set by meson via -DMD_MCP_BRIDGE_VERSION=... */
#ifndef MD_MCP_BRIDGE_VERSION
#define MD_MCP_BRIDGE_VERSION "0.1.0"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MD_MCP_NOTIFY_COALESCE_MS
#define MD_MCP_NOTIFY_COALESCE_MS 50
#endif

/* Opaque bridge context */
typedef struct MdMcpBridge MdMcpBridge;

/* MCP defaults to compact without changing binary-session format values. */
typedef enum {
    MD_MCP_TREE_FORMAT_DEFAULT,
    MD_MCP_TREE_FORMAT_COMPACT,
    MD_MCP_TREE_FORMAT_JSON,
} MdMcpTreeFormat;

/* Configuration for creating an MCP bridge */
typedef struct {
    MdA11yCtx    *a11y;           /* shared a11y context (required)     */
    MdInput      *input;          /* shared input context (optional)    */
    MdMcpTreeFormat tree_format;  /* resource only; DEFAULT=compact, actions=JSON */
    /* 0 = MD_MCP_NOTIFY_COALESCE_MS; nonzero windows capped at 200 ms. */
    uint32_t      notify_coalesce_ms;
    bool          notify_sync;    /* explicit opt-out: deliver hints synchronously */
    uint32_t      settle_ms;      /* settle time after actions (0=default) */

    /* Transport: use one of these */
    int           stdio_in_fd;    /* stdio transport: read fd (-1 to skip) */
    int           stdio_out_fd;   /* stdio transport: write fd             */
    uint16_t      http_port;      /* HTTP+SSE transport (0 to skip)        */
    const char   *http_bind_addr; /* NULL for localhost only               */
    const char   *http_token_file;
    const char *const *http_public_urls;
    size_t        http_public_url_count;
    const char *const *http_allow_npubs;
    size_t        http_allow_npub_count;
    const char   *http_allow_file;
    /* Live NIP-98 authorizer; see MdMcpHttpConfig.nip98_authorize. */
    bool        (*http_nip98_authorize)(const char *pubkey_hex, void *userdata);
    void         *http_nip98_authorize_userdata;
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

/* Host lifecycle: run with a dedicated SIGINT/SIGTERM sigwait thread and
 * accessibility-liveness supervision. Returns -1 on desktop loss/error.
 * Caller MUST block SIGINT/SIGTERM before creating any threads (including
 * backend/bridge creation). All supervisor threads are joined before return;
 * caller still owns bridge destruction and borrowed a11y/input contexts. */
int md_mcp_bridge_run_supervised(MdMcpBridge *bridge);

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
