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
 * lookup in the newest replaceable event the bridge has accepted. Each
 * accepted replacement (and each readiness transition) also wakes one watcher
 * thread, which tells the MCP transport to re-read its live streams' grants
 * (md-ht7: tools/list_changed to exactly the streams whose tool set changed).
 * While history is pending or every relay is down the source answers
 * MD_MCP_GRANT_UNKNOWN: requests are refused and open streams are held, sent
 * nothing, until the next LOADED wake-up re-evaluates them (md-554). Once
 * every relay has refused the subscription with a terminal CLOSED (FAILED,
 * md-ip2k) the answer is a known deny instead: held streams close on that
 * wake-up and the log names each relay and its reason. A relay that later
 * serves history again (e.g. after a re-challenge) reloads the list as usual.
 */
#include "host_mcp.h"
#include "accounts.h"
#include "mcp_http.h"
#include "nostr.h"
#include "session.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct HostMcpAcl {
    MdAccountStartup account;
    MdNostr *nostr;
    pthread_t watch_thread;
    bool watch_started;
    /* One change listener (the MCP transport). watch_mu is held while it
     * runs, so unregistering waits out an in-flight call. */
    pthread_mutex_t watch_mu;
    void (*watch_fn)(void *);
    void *watch_ud;
};

static void acl_notify(HostMcpAcl *acl) {
    pthread_mutex_lock(&acl->watch_mu);
    if (acl->watch_fn) acl->watch_fn(acl->watch_ud);
    pthread_mutex_unlock(&acl->watch_mu);
}

static void acl_owner_revoked(void *userdata) {
    (void)userdata;
    /* The bridge has already stopped itself: every live grant now denies. */
    fprintf(stderr, "host[mcp]: host role changed; host-allowlist NIP-98 grants now deny\n");
}

/* On each readiness transition and accepted list replacement: notify the
 * change listener, then report it for operators. Logging second makes the
 * log line a barrier (the e2e test's) for the listener's notifications.
 * Waits on the bridge's condition variable, woken by EOSE/CLOSED/connection
 * boundaries and accepted newer events; it is not a readiness gate
 * (host_mcp_acl_grant() already denies unless LOADED) and never a timer. */
static void *acl_watch_thread(void *arg) {
    HostMcpAcl *acl = arg;
    MdNostrAclState state = MD_NOSTR_ACL_UNKNOWN, logged = MD_NOSTR_ACL_UNKNOWN;
    uint64_t version = 0;
    while (md_nostr_allowlist_wait_change(acl->nostr, &state, &version) == 0) {
        acl_notify(acl);
        if (state == MD_NOSTR_ACL_LOADED) {
            MdAllowlistEntry *entries = NULL;
            size_t count = 0, agents = 0, inputs = 0;
            if (md_nostr_allowlist_snapshot(acl->nostr, &entries, &count) != 0) continue;
            for (size_t i = 0; i < count; i++) {
                uint32_t caps = 0;
                if (md_nostr_parse_caps(entries[i].caps, &caps) || !(caps & MD_CAP_AGENT))
                    continue;
                agents++;
                if (caps & MD_CAP_INPUT) inputs++;
            }
            free(entries);
            fprintf(stderr, "host[mcp]: host allowlist %s (v%llu): %zu member(s), "
                            "%zu with agent (%zu also input)\n",
                    logged == MD_NOSTR_ACL_LOADED ? "updated" : "loaded",
                    (unsigned long long)version, count, agents, inputs);
        } else if (state == MD_NOSTR_ACL_FAILED && state != logged) {
            char why[1024];
            if (!md_nostr_allowlist_refusals(acl->nostr, why, sizeof(why)))
                snprintf(why, sizeof(why), "no allowlist subscription was issued");
            fprintf(stderr, "host[mcp]: host allowlist refused by every relay (%s); "
                            "host-allowlist NIP-98 requests are refused (401) and their "
                            "SSE streams closed until a relay delivers it\n", why);
        } else if (state != logged) {
            fprintf(stderr, "host[mcp]: host allowlist history pending; "
                            "host-allowlist NIP-98 grants deny and their SSE streams "
                            "are held until a relay delivers it\n");
        } else {
            continue;
        }
        logged = state;
    }
    /* Stopped, or the host role changed: every grant now denies. */
    acl_notify(acl);
    return NULL;
}

HostMcpAcl *host_mcp_acl_start(const char *const *relay_urls, int relay_count) {
    HostMcpAcl *acl = calloc(1, sizeof(*acl));
    if (!acl) return NULL;
    pthread_mutex_init(&acl->watch_mu, NULL);
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
    acl->watch_started = pthread_create(&acl->watch_thread, NULL, acl_watch_thread, acl) == 0;
    if (!acl->watch_started) {
        fprintf(stderr, "host[mcp]: could not start the host allowlist watcher\n");
        host_mcp_acl_stop(acl);
        return NULL;
    }
    fprintf(stderr, "host[mcp]: NIP-98 also authorized by host allowlist "
                    "(agent grant; action tools need input) for %.*s... on %d relay(s)\n", 12, md_nostr_get_npub(acl->nostr), relay_count);
    return acl;
}

uint32_t host_mcp_acl_grant(const char *pubkey_hex, void *userdata) {
    HostMcpAcl *acl = userdata;
    if (!acl || !acl->nostr) return 0;
    /* MCP has no video: only the agent (admission) and input (actions) bits.
     * Grant and readiness are read together, so a stopped bridge (host role
     * changed) or every relay refusing the subscription denies, while relays
     * being down (or awaiting AUTH) is only unknown. */
    bool known = true;
    uint32_t caps = md_nostr_granted_capabilities_known(acl->nostr, pubkey_hex,
                                                        MD_CAP_AGENT | MD_CAP_INPUT, &known);
    return known ? caps : MD_MCP_GRANT_UNKNOWN;
}

int host_mcp_acl_watch(void *userdata, void (*on_change)(void *), void *on_change_ud) {
    HostMcpAcl *acl = userdata;
    if (!acl) return -1;
    pthread_mutex_lock(&acl->watch_mu); /* waits for an in-flight on_change */
    int rc = on_change && acl->watch_fn ? -1 : 0;
    if (!rc) {
        acl->watch_fn = on_change;
        acl->watch_ud = on_change ? on_change_ud : NULL;
    }
    pthread_mutex_unlock(&acl->watch_mu);
    return rc;
}

void host_mcp_acl_stop(HostMcpAcl *acl) {
    if (!acl) return;
    md_nostr_stop(acl->nostr); /* wakes the watcher's change wait */
    if (acl->watch_started) pthread_join(acl->watch_thread, NULL);
    md_nostr_destroy(acl->nostr);
    md_account_startup_clear(&acl->account);
    pthread_mutex_destroy(&acl->watch_mu);
    free(acl);
}
