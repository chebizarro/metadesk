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

typedef struct HostMcpAcl HostMcpAcl;

/* Restore the saved host role and open a minimal Nostr bridge whose only
 * subscription is that role's kind-30000 metadesk-allowlist (no DMs, no
 * session log). Returns NULL, having printed why, if no host role is saved,
 * restore fails or no relay is usable. Never blocks on relay history:
 * authorization fails closed until EOSE. Caller must have blocked
 * SIGINT/SIGTERM already (bridge threads inherit the mask). */
HostMcpAcl *host_mcp_acl_start(const char *const *relay_urls, int relay_count);

/* MdMcpHttpConfig.nip98_authorize: true only if the live, loaded list grants
 * pubkey_hex the agent capability (md-ikr v1 format). */
bool host_mcp_acl_authorize(const char *pubkey_hex, void *acl);

/* Stop the bridge, join the readiness logger and release the host role. */
void host_mcp_acl_stop(HostMcpAcl *acl);

/* `metadesk-host mcp-proxy ...`; argv excludes the subcommand name. */
int md_host_mcp_proxy_cli(int argc, char **argv);

#endif /* MD_HOST_MCP_H */
