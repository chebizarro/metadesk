/*
 * metadesk — host_mcp.h
 * Host-binary MCP helpers kept out of main.c:
 *   - the live NIP-98 authorization source backed by the saved host role's
 *     NIP-51 metadesk-allowlist (md-ah9.3), and
 *   - the `metadesk-host mcp-proxy` stdio<->HTTP NIP-98 signing shim
 *     (md-ah9.14).
 */
#ifndef MD_HOST_MCP_H
#define MD_HOST_MCP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct HostMcpAcl HostMcpAcl;

/* Restore the saved host role and open a minimal Nostr bridge whose only
 * subscription is that role's kind-30000 metadesk-allowlist (no DMs, no
 * session log). Returns NULL, having printed why, if no host role is saved,
 * restore fails or no relay is usable. Never blocks on relay history:
 * authorization fails closed until EOSE. Caller must have blocked
 * SIGINT/SIGTERM already (bridge threads inherit the mask). */
HostMcpAcl *host_mcp_acl_start(const char *const *relay_urls, int relay_count);

/* MdMcpHttpConfig.nip98_grant: pubkey_hex's agent/input grant from the live,
 * loaded list (md-ikr v1 format); 0 when unlisted or once the bridge has
 * stopped; MD_MCP_GRANT_UNKNOWN while history is pending or every relay is
 * down (md-554: requests deny, open streams are held). The MCP transport
 * admits only a grant with agent and gates actions on input. */
uint32_t host_mcp_acl_grant(const char *pubkey_hex, void *acl);

/* MdMcpHttpConfig.nip98_grant_watch: register on_change (one listener;
 * -1 if one is already registered) or, with NULL, unregister after any
 * in-flight call returns. on_change runs on the bridge's watcher thread after
 * each readiness transition or accepted list replacement, before that change
 * is logged, and once more when the bridge stops (every grant then denies).
 * Event-driven via md_nostr_allowlist_wait_change(); never a timer. */
int host_mcp_acl_watch(void *acl, void (*on_change)(void *), void *on_change_ud);

/* Stop the bridge, join the watcher and release the host role. Unregister
 * the listener first, or keep it valid until this returns. */
void host_mcp_acl_stop(HostMcpAcl *acl);

/* `metadesk-host mcp-proxy ...`; argv excludes the subcommand name. */
int md_host_mcp_proxy_cli(int argc, char **argv);

#endif /* MD_HOST_MCP_H */
