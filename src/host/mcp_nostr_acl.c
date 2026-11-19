/*
 * metadesk — mcp_nostr_acl.c
 * Live NIP-98 MCP authorization from the host role's NIP-51 allowlist
 * (md-ah9.3, container-runtime plan §7.7 item 4).
 *
 * Cost, made explicit: enabling this source in MCP mode restores the saved
 * host role (its signer must be reachable; signing is never requested),
 * opens one WebSocket per --relay and issues exactly one REQ
 * {kinds:[30000], authors:[host], #d:[metadesk-allowlist], limit:1} that is
 * kept open for live replacements. No gift-wrap DM subscription, session log,
 * capture, polling or refresh timer is added. Authorization is a lock-guarded
 * lookup in the newest replaceable event the bridge has accepted.
 */
#include "host_mcp.h"
#include "accounts.h"
#include "nostr.h"
#include "session.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct HostMcpAcl {
    MdAccountStartup account;
    MdNostr *nostr;
    pthread_t ready_thread;
    bool ready_started;
};

static void acl_owner_revoked(void *userdata) {
    (void)userdata;
    /* The bridge has already stopped itself: every live grant now denies. */
    fprintf(stderr, "host[mcp]: host role changed; host-allowlist NIP-98 grants now deny\n");
}

/* Report readiness transitions and each accepted list replacement for
 * operators (and as the e2e test's barrier). Waits on the bridge's condition
 * variable, woken by EOSE/CLOSED/connection boundaries and accepted newer
 * events; it is not a readiness gate (authorize() already denies unless
 * LOADED) and never a timer. */
static void *acl_ready_logger(void *arg) {
    HostMcpAcl *acl = arg;
    MdNostrAclState state = MD_NOSTR_ACL_UNKNOWN, logged = MD_NOSTR_ACL_UNKNOWN;
    uint64_t version = 0;
    while (md_nostr_allowlist_wait_change(acl->nostr, &state, &version) == 0) {
        if (state == MD_NOSTR_ACL_LOADED) {
            MdAllowlistEntry *entries = NULL;
            size_t count = 0, agents = 0;
            if (md_nostr_allowlist_snapshot(acl->nostr, &entries, &count) != 0) continue;
            for (size_t i = 0; i < count; i++) {
                uint32_t caps = 0;
                if (!md_nostr_parse_caps(entries[i].caps, &caps) && (caps & MD_CAP_AGENT))
                    agents++;
            }
            free(entries);
            fprintf(stderr, "host[mcp]: host allowlist %s (v%llu): %zu member(s), %zu with agent\n",
                    logged == MD_NOSTR_ACL_LOADED ? "updated" : "loaded",
                    (unsigned long long)version, count, agents);
        } else if (state != logged) {
            fprintf(stderr, "host[mcp]: host allowlist history %s; "
                            "host-allowlist NIP-98 grants deny until a relay delivers it\n",
                    state == MD_NOSTR_ACL_FAILED ? "unavailable" : "pending");
        } else {
            continue;
        }
        logged = state;
    }
    return NULL;
}

HostMcpAcl *host_mcp_acl_start(const char *const *relay_urls, int relay_count) {
    HostMcpAcl *acl = calloc(1, sizeof(*acl));
    if (!acl) return NULL;
    int rc = md_account_startup_open(MD_ACCOUNT_HOST, &acl->account);
    if (rc || !acl->account.binding) {
        fprintf(stderr, "host[mcp]: --mcp-allow-host-list needs a saved host role: %s\n",
                md_accounts_error_string(rc ? rc : MD_ACCOUNTS_NOT_FOUND));
        host_mcp_acl_stop(acl);
        return NULL;
    }
    MdNostrCallbacks cbs = { .on_owner_revoked = acl_owner_revoked };
    MdNostrConfig cfg = {
        .signer = md_account_binding_signer(acl->account.binding),
        .owner = acl->account.binding,
        .relay_urls = (const char **)relay_urls,
        .relay_count = relay_count,
    };
    acl->nostr = md_nostr_create(&cfg, &cbs);
    if (!acl->nostr) {
        fprintf(stderr, "host[mcp]: could not start the host allowlist relay bridge\n");
        host_mcp_acl_stop(acl);
        return NULL;
    }
    acl->ready_started = pthread_create(&acl->ready_thread, NULL, acl_ready_logger, acl) == 0;
    fprintf(stderr, "host[mcp]: NIP-98 also authorized by host allowlist (agent grant) "
                    "for %.*s... on %d relay(s)\n", 12, md_nostr_get_npub(acl->nostr), relay_count);
    return acl;
}

bool host_mcp_acl_authorize(const char *pubkey_hex, void *userdata) {
    HostMcpAcl *acl = userdata;
    return acl && acl->nostr &&
        md_nostr_granted_capabilities(acl->nostr, pubkey_hex, MD_CAP_AGENT) == MD_CAP_AGENT;
}

void host_mcp_acl_stop(HostMcpAcl *acl) {
    if (!acl) return;
    md_nostr_stop(acl->nostr); /* wakes the logger's history wait */
    if (acl->ready_started) pthread_join(acl->ready_thread, NULL);
    md_nostr_destroy(acl->nostr);
    md_account_startup_clear(&acl->account);
    free(acl);
}
