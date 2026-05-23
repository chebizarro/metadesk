/*
 * fips-nat — publish.c
 * Legacy NAT endpoint publication to Nostr relays.
 *
 * DEPRECATED: kind:30078 NAT endpoint JSON is retained only for the
 * legacy fips-nat path. FIPS v0.3+ overlay adverts and traversal
 * signaling are the recommended discovery/reachability mechanism.
 *
 * Publishes the node's STUN-discovered reflexive address as a
 * kind:30078 addressable event via the legacy transport publisher.
 *
 * The JSON content format:
 *   {
 *     "v": 1,
 *     "ip": "203.0.113.42",
 *     "port": 54321,
 *     "proto": "udp",
 *     "fips_port": 2121,
 *     "punch_port": 19800
 *   }
 *
 * Legacy subscribing peers parse this to know where to send punch probes.
 */
#include "publish.h"

#include <cjson/cJSON.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── JSON serialization ──────────────────────────────────────── */

char *md_nat_endpoint_to_json(const MdNatEndpoint *ep) {
    if (!ep || ep->stun.ip[0] == '\0')
        return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "v", MD_PUBLISH_VERSION);
    cJSON_AddStringToObject(root, "ip", ep->stun.ip);
    cJSON_AddNumberToObject(root, "port", ep->stun.port);
    cJSON_AddStringToObject(root, "proto", "udp");

    if (ep->stun.is_ipv6)
        cJSON_AddTrueToObject(root, "ipv6");

    if (ep->fips_port > 0)
        cJSON_AddNumberToObject(root, "fips_port", ep->fips_port);

    if (ep->punch_port > 0)
        cJSON_AddNumberToObject(root, "punch_port", ep->punch_port);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

int md_nat_endpoint_from_json(const char *json, MdNatEndpoint *ep) {
    if (!json || !ep)
        return -1;

    memset(ep, 0, sizeof(*ep));

    cJSON *root = cJSON_Parse(json);
    if (!root)
        return -1;

    /* Version check */
    cJSON *v = cJSON_GetObjectItem(root, "v");
    if (!v || !cJSON_IsNumber(v) || v->valueint < 1) {
        cJSON_Delete(root);
        return -1;
    }

    /* IP (required) */
    cJSON *ip = cJSON_GetObjectItem(root, "ip");
    if (!ip || !cJSON_IsString(ip) || !ip->valuestring[0]) {
        cJSON_Delete(root);
        return -1;
    }
    strncpy(ep->stun.ip, ip->valuestring, sizeof(ep->stun.ip) - 1);

    /* Port (required) */
    cJSON *port = cJSON_GetObjectItem(root, "port");
    if (!port || !cJSON_IsNumber(port) || port->valueint <= 0) {
        cJSON_Delete(root);
        return -1;
    }
    ep->stun.port = (uint16_t)port->valueint;

    /* IPv6 flag (optional) */
    cJSON *ipv6 = cJSON_GetObjectItem(root, "ipv6");
    ep->stun.is_ipv6 = (ipv6 && cJSON_IsTrue(ipv6));

    /* FIPS port (optional) */
    cJSON *fp = cJSON_GetObjectItem(root, "fips_port");
    if (fp && cJSON_IsNumber(fp))
        ep->fips_port = (uint16_t)fp->valueint;

    /* Punch port (optional) */
    cJSON *pp = cJSON_GetObjectItem(root, "punch_port");
    if (pp && cJSON_IsNumber(pp))
        ep->punch_port = (uint16_t)pp->valueint;

    cJSON_Delete(root);
    return 0;
}

/* ── Nostr publication ───────────────────────────────────────── */

int md_publish_nat_endpoint(MdNostr *nostr, const MdNatEndpoint *ep) {
    if (!nostr || !ep)
        return -1;

    /* Serialize endpoint to JSON */
    char *content = md_nat_endpoint_to_json(ep);
    if (!content)
        return -1;

    /* Deprecated legacy publication via the core transport publisher
     * (kind:30078, d:"fips-transport"). Do not use this as the recommended
     * metadesk/FIPS discovery path; the FIPS daemon owns overlay adverts,
     * STUN/TURN, and traversal signaling. */
    int ret = md_nostr_publish_transport(nostr, content);
    free(content);

    if (ret == 0) {
        fprintf(stderr, "publish: NAT endpoint published: %s:%u\n",
                ep->stun.ip, ep->stun.port);
    }

    return ret;
}

int md_subscribe_nat_endpoint(MdNostr *nostr, const char *peer_pubkey_hex) {
    if (!nostr || !peer_pubkey_hex)
        return -1;

    /* Subscribe via the existing transport subscription mechanism.
     * The on_transport callback will fire with the JSON content.
     * The caller parses it with md_nat_endpoint_from_json(). */
    return md_nostr_subscribe_transport(nostr, peer_pubkey_hex);
}

/* ── Legacy API ──────────────────────────────────────────────── */

int md_publish_transport(MdNostr *nostr, const char *fips_addr) {
    if (!nostr || !fips_addr) return -1;
    return md_nostr_publish_transport(nostr, fips_addr);
}
