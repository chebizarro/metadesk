#include "relay_plan.h"
#include "discovery_http.h"
#include "md_time.h"
#include <curl/curl.h>
#include <nostr-event.h>
#include <nostr-kinds.h>
#include <nostr/nip11/nip11.h>
#include <nostr/nip17/nip17.h>
#include <nostr/nip65/nip65.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *relay_url(const char *url, bool explicit, uint64_t deadline_ns) {
    if (!url || strlen(url) > MD_RELAY_URL_MAX || !strstr(url, "://") ||
        strpbrk(url, "\\?#") || !md_discovery_http_init()) return NULL;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 0x20 || *p >= 0x7f) return NULL;
    const char *authority = strstr(url, "://") + 3;
    /* Reject parser ambiguities in the authority (including userinfo and zone IDs). */
    if (!*authority || *authority == '/' ||
        strcspn(authority, "@%/") < strcspn(authority, "/")) return NULL;

    CURLU *uri = curl_url();
    char *scheme = NULL, *host = NULL, *port = NULL, *canonical = NULL, *out = NULL;
    if (!uri || curl_url_set(uri, CURLUPART_URL, url, CURLU_NON_SUPPORT_SCHEME | CURLU_DISALLOW_USER) ||
        curl_url_get(uri, CURLUPART_SCHEME, &scheme, 0) ||
        (strcmp(scheme, "wss") && (!explicit || strcmp(scheme, "ws"))) ||
        curl_url_get(uri, CURLUPART_HOST, &host, 0)) goto done;
    if (!curl_url_get(uri, CURLUPART_PORT, &port, 0)) {
        if (!strcmp(port, "0")) goto done;
        if ((!strcmp(scheme, "wss") && !strcmp(port, "443")) ||
            (!strcmp(scheme, "ws") && !strcmp(port, "80")))
            if (curl_url_set(uri, CURLUPART_PORT, NULL, 0)) goto done;
    }
    if (!explicit && deadline_ns) {
        /* CURLU includes brackets around IPv6 literals; getaddrinfo does not. */
        const char *name = host;
        if (host[0] == '[') {
            host[strlen(host) - 1] = '\0';
            name++;
        }
        if (!md_discovery_public_host(name, deadline_ns)) goto done;
    }
    if (!curl_url_get(uri, CURLUPART_URL, &canonical, 0))
        out = nostr_nip65_normalize_url(canonical);
 done:
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_free(canonical);
    curl_url_cleanup(uri);
    return out;
}

bool md_relay_target_check(const MdRelayTarget *target, uint64_t deadline_ns) {
    if (!deadline_ns) deadline_ns = md_monotonic_ns() + 10000000000ULL;
    char *url = target ? relay_url(target->url, target->source == MD_RELAY_CONFIGURED, deadline_ns) : NULL;
    bool valid = url != NULL;
    free(url);
    return valid;
}

bool md_relay_plan_add(MdRelayPlan *plan, const char *url, MdRelaySource source) {
    if (!plan->deadline_ns) plan->deadline_ns = md_monotonic_ns() + 10000000000ULL;
    char *canonical = relay_url(url, source == MD_RELAY_CONFIGURED, plan->deadline_ns);
    if (!canonical) return false;
    for (size_t i = 0; i < plan->count; ++i) {
        if (!strcmp(plan->target[i].url, canonical)) {
            free(canonical);
            return true;
        }
    }
    if (plan->count == MD_RELAY_TARGET_MAX) {
        free(canonical);
        return false;
    }
    plan->target[plan->count++] = (MdRelayTarget){canonical, source};
    return true;
}

void md_relay_plan_clear(MdRelayPlan *plan) {
    for (size_t i = 0; i < plan->count; ++i) free(plan->target[i].url);
    memset(plan, 0, sizeof(*plan));
}

const char *md_relay_source_name(MdRelaySource source) {
    switch (source) {
    case MD_RELAY_CONFIGURED: return "configured";
    case MD_RELAY_NIP05: return "NIP-05";
    case MD_RELAY_NPROFILE: return "nprofile";
    case MD_RELAY_NIP65: return "NIP-65";
    case MD_RELAY_DM_LIST: return "DM list";
    }
    return "unknown";
}

bool md_relay_metadata_offer(MdRelayMetadata *metadata, const char *author, const NostrEvent *event) {
    if (!event || !author) return false;
    int kind = nostr_event_get_kind(event);
    NostrEvent **slot = kind == NOSTR_KIND_RELAY_LIST_METADATA ? &metadata->nip65 :
                        kind == NOSTR_KIND_DM_RELAY_LIST ? &metadata->dm : NULL;
    const char *pubkey = nostr_event_get_pubkey(event);
    char id[65];
    if (!slot || !pubkey || strcmp(author, pubkey) ||
        nostr_event_validate(event, id) != NOSTR_EVENT_VALIDATION_OK) return false;
    if (*slot) {
        int64_t old = nostr_event_get_created_at(*slot), new_time = nostr_event_get_created_at(event);
        if (new_time <= old) {
            char *old_id = nostr_event_get_id(*slot);
            bool keep = new_time < old || !old_id || strcmp(id, old_id) >= 0;
            free(old_id);
            if (keep) return false;
        }
    }
    NostrEvent *copy = nostr_event_copy(event);
    if (!copy) return false;
    nostr_event_free(*slot);
    *slot = copy;
    return true;
}

void md_relay_metadata_clear(MdRelayMetadata *metadata) {
    nostr_event_free(metadata->nip65);
    nostr_event_free(metadata->dm);
    memset(metadata, 0, sizeof(*metadata));
}

static void add_urls(MdRelayPlan *out, char **urls, size_t count, MdRelaySource source,
                     const MdRelayPlan *bootstrap) {
    for (size_t i = 0; i < count && out->count < MD_RELAY_TARGET_MAX; ++i) {
        /* Explicit approval is for an exact URL, not its host or any URL the
         * metadata may name. Metadata can assign a role to that same relay. */
        char *canonical = relay_url(urls[i], true, 0);
        MdRelaySource provenance = source;
        if (canonical) for (size_t b = 0; b < bootstrap->count; ++b)
            if (bootstrap->target[b].source == MD_RELAY_CONFIGURED &&
                !strcmp(canonical, bootstrap->target[b].url)) {
                provenance = MD_RELAY_CONFIGURED;
                break;
            }
        if (canonical) md_relay_plan_add(out, canonical, provenance);
        free(canonical);
    }
}

static void add_bootstrap(MdRelayPlan *out, const MdRelayPlan *bootstrap) {
    for (size_t i = 0; i < bootstrap->count; ++i)
        md_relay_plan_add(out, bootstrap->target[i].url, bootstrap->target[i].source);
}

static void add_hints(MdRelayPlan *out, const MdIdentity *address) {
    if (!address || address->hint_source == MD_HINT_NONE) return;
    for (size_t i = 0; i < address->hint_count && out->count < MD_RELAY_TARGET_MAX; ++i)
        md_relay_plan_add(out, address->hints[i], address->hint_source == MD_HINT_NIP05 ?
                          MD_RELAY_NIP05 : MD_RELAY_NPROFILE);
}

bool md_relay_plan_profile_bootstrap(MdRelayPlan *plan, const MdIdentity *address) {
    if (!plan || plan->count || !address || address->hint_source != MD_HINT_NPROFILE) return false;
    add_hints(plan, address);
    return plan->count != 0;
}

static void add_nip65(MdRelayPlan *out, const MdRelayMetadata *metadata, bool read, bool write,
                      const MdRelayPlan *bootstrap) {
    NostrRelayList *list = NULL;
    if (!metadata || !metadata->nip65 || nostr_nip65_parse_relay_list(metadata->nip65, &list)) return;
    size_t count = 0;
    if (read) {
        char **urls = nostr_nip65_get_read_relays(list, &count);
        if (urls) add_urls(out, urls, count, MD_RELAY_NIP65, bootstrap);
        nostr_nip65_free_string_array(urls);
    }
    if (write) {
        char **urls = nostr_nip65_get_write_relays(list, &count);
        if (urls) add_urls(out, urls, count, MD_RELAY_NIP65, bootstrap);
        nostr_nip65_free_string_array(urls);
    }
    nostr_nip65_list_free(list);
}

static void add_dm(MdRelayPlan *out, const MdRelayMetadata *metadata, const MdRelayPlan *bootstrap) {
    if (!metadata || !metadata->dm) return;
    NostrDmRelayList *list = nostr_nip17_parse_dm_relay_list(metadata->dm);
    if (list) add_urls(out, list->relays, list->count, MD_RELAY_DM_LIST, bootstrap);
    nostr_nip17_free_dm_relay_list(list);
}

MdRelayPlanStatus md_relay_plan_build(MdRelayPlan *out, MdRelayRole role,
    const MdRelayPlan *bootstrap, const MdIdentity *address, const MdRelayMetadata *metadata) {
    if (!bootstrap || !bootstrap->count) return MD_RELAY_NO_BOOTSTRAP;
    switch (role) {
    case MD_RELAY_BOOTSTRAP: add_bootstrap(out, bootstrap); break;
    case MD_RELAY_DISCOVERY: add_bootstrap(out, bootstrap); add_hints(out, address); break;
    case MD_RELAY_STORE:
        add_nip65(out, metadata, false, true, bootstrap);
        if (!out->count) add_bootstrap(out, bootstrap);
        break;
    case MD_RELAY_FETCH: add_bootstrap(out, bootstrap); add_nip65(out, metadata, true, true, bootstrap); break;
    case MD_RELAY_DM_IN: add_dm(out, metadata, bootstrap); break;
    case MD_RELAY_DM_OUT:
        add_dm(out, metadata, bootstrap);
        if (!out->count) add_nip65(out, metadata, true, false, bootstrap);
        if (!out->count) { add_hints(out, address); add_bootstrap(out, bootstrap); }
        break;
    }
    return out->count ? MD_RELAY_READY : MD_RELAY_NO_TARGETS;
}

size_t md_relay_plan_union(const MdRelayPlan *plans, const unsigned *roles,
                           size_t count, MdRelayUse out[MD_RELAY_CONNECTION_MAX]) {
    size_t selected = 0;
    /* First give every nonempty role a target, then distribute the remainder. */
    for (size_t i = 0; i < MD_RELAY_TARGET_MAX; ++i) for (size_t p = 0; p < count; ++p) {
        if (i >= plans[p].count) continue;
        const MdRelayTarget *t = &plans[p].target[i];
        size_t j = 0;
        while (j < selected && strcmp(out[j].target->url, t->url)) j++;
        if (j == selected) {
            if (selected == MD_RELAY_CONNECTION_MAX) continue;
            out[selected++] = (MdRelayUse){t, 0};
        }
        if (t->source == MD_RELAY_CONFIGURED) out[j].target = t;
        out[j].mask |= roles[p];
    }
    return selected;
}

MdRelayAuth md_relay_auth_from_json(const char *json) {
    RelayInformationDocument *info = json ? nostr_nip11_parse_info(json) : NULL;
    if (!info) return MD_RELAY_AUTH_UNKNOWN;
    MdRelayAuth auth = info->limitation && info->limitation->auth_required ?
        MD_RELAY_AUTH_REQUIRED : MD_RELAY_AUTH_NOT_ADVERTISED;
    nostr_nip11_free_info(info);
    return auth;
}

MdRelayAuth md_relay_probe_auth(const MdRelayTarget *target) {
    bool explicit = target && target->source == MD_RELAY_CONFIGURED;
    char *url = target ? relay_url(target->url, explicit, 0) : NULL;
    if (!url) return MD_RELAY_AUTH_UNKNOWN;
    char http[MD_RELAY_URL_MAX + 8];
    bool tls = !strncmp(url, "wss://", 6);
    snprintf(http, sizeof(http), "%s://%s", tls ? "https" : "http", url + (tls ? 6 : 5));
    free(url);
    /* Do not let an operator-approved local URL delegate trust via redirects. */
    char *json = md_discovery_http_get(http, "Accept: application/nostr+json", !explicit, 3000, NULL);
    MdRelayAuth auth = md_relay_auth_from_json(json);
    free(json);
    return auth;
}
