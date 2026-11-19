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
/* A nip98_grant result meaning "this source cannot answer right now" (md-554):
 * e.g. its history is pending or every relay is down. Not a grant: requests
 * are refused as for 0, but open SSE streams the source admitted are held
 * (kept open, sent nothing) instead of closed, until a known answer. Only
 * for outages the source expects to end: once it knows no answer is coming
 * (e.g. every relay refused its subscription, md-ip2k) it must answer 0 and
 * call on_change, so held streams close rather than wait indefinitely. */
#define MD_MCP_GRANT_UNKNOWN 0x80000000u

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
     * fail closed: 0 or MD_MCP_GRANT_UNKNOWN (alone) while its source is not
     * ready. Rechecked before every SSE broadcast so a revoked (0) stream is
     * dropped without a restart; an UNKNOWN stream is skipped and held. */
    uint32_t    (*nip98_grant)(const char *pubkey_hex, void *userdata);
    void         *nip98_grant_userdata;
    /* Optional change feed for nip98_grant (md-ht7). With both set, create
     * registers watch(nip98_grant_userdata, on_change, on_change_ud) and
     * destroy unregisters with watch(nip98_grant_userdata, NULL, NULL)
     * before freeing anything; unregistering must wait out an in-flight
     * on_change. watch returns 0 on success (create fails otherwise). The
     * source calls on_change once per accepted grant change, event-driven
     * (never a timer), from a thread it owns and not under its grant lock.
     * The transport then re-reads every live SSE stream's grant: a stream
     * that lost agent is closed, one whose answer is MD_MCP_GRANT_UNKNOWN is
     * held, and one whose visible tool set changed since its last known
     * grant gets one notifications/tools/list_changed. A source that can
     * return UNKNOWN must also call on_change when it can answer again, so
     * held streams are re-evaluated. initialize then advertises
     * capabilities.tools.listChanged. */
    int         (*nip98_grant_watch)(void *userdata,
                                     void (*on_change)(void *on_change_ud),
                                     void *on_change_ud);
    int           max_clients;   /* max concurrent SSE clients (default: 4); at
                                  * the limit, a stream not admitted by
                                  * nip98_grant takes the slot of one held
                                  * (MD_MCP_GRANT_UNKNOWN) stream */
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

/* Send an SSE event to all connected clients (live-admitted clients whose
 * grant lost agent are closed instead, and held ones skipped). Used by the
 * server's write_fn. */
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
