/*
 * fips-nat — publish.h
 * Legacy NAT endpoint publication to Nostr relays.
 *
 * DEPRECATED: retained only for the legacy fips-nat path. Current
 * metadesk/FIPS deployments should use the FIPS daemon's discovery,
 * STUN/TURN, and traversal signaling instead of kind:30078 NAT JSON.
 *
 * Publishes the node's STUN-discovered reflexive (public) address
 * as legacy kind:30078 JSON for old UDP hole-punch peers.
 *
 * Event content is JSON:
 *   { "v":1, "ip":"...", "port":N, "proto":"udp",
 *     "fips_port":2121, "punch_port":N }
 */
#ifndef MD_PUBLISH_H
#define MD_PUBLISH_H

#include "stun.h"
#include "nostr.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────────────────────────────────────────── */

#define MD_PUBLISH_D_TAG         "fips-nat-endpoint"
#define MD_PUBLISH_EVENT_KIND    30078
#define MD_PUBLISH_VERSION       1

/* ── NAT endpoint info ───────────────────────────────────────── */

typedef struct {
    MdStunResult  stun;            /* reflexive address from STUN      */
    uint16_t      fips_port;       /* FIPS daemon port (default 2121)  */
    uint16_t      punch_port;      /* local port for hole punch probes */
} MdNatEndpoint;

/* ── Serialization (JSON) ────────────────────────────────────── */

/*
 * Serialize a NAT endpoint to JSON string.
 * Returns malloc'd JSON string, caller must free.
 * Returns NULL on error.
 */
char *md_nat_endpoint_to_json(const MdNatEndpoint *ep);

/*
 * Deserialize a NAT endpoint from JSON string.
 * Returns 0 on success, -1 on parse error.
 */
int md_nat_endpoint_from_json(const char *json, MdNatEndpoint *ep);

/* ── Publication ─────────────────────────────────────────────── */

/*
 * Publish our legacy NAT endpoint JSON to Nostr relays.
 * This is not the recommended FIPS discovery/reachability path.
 *
 * nostr: active Nostr bridge (connected to relays with a signer)
 * ep:    endpoint to publish
 *
 * Returns 0 on success, -1 on error.
 */
int md_publish_nat_endpoint(MdNostr *nostr, const MdNatEndpoint *ep);

/*
 * Subscribe to a peer's legacy NAT endpoint updates.
 * Results arrive via the on_transport callback with the JSON content.
 *
 * nostr:          active Nostr bridge
 * peer_pubkey_hex: 64-char hex pubkey of the peer
 *
 * Returns 0 on success, -1 on error.
 */
int md_subscribe_nat_endpoint(MdNostr *nostr, const char *peer_pubkey_hex);

/*
 * Legacy API: publish FIPS overlay address (delegates to core).
 * Kept for backward compatibility.
 */
int md_publish_transport(MdNostr *nostr, const char *fips_addr);

#ifdef __cplusplus
}
#endif

#endif /* MD_PUBLISH_H */
