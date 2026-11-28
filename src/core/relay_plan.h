#ifndef MD_RELAY_PLAN_H
#define MD_RELAY_PLAN_H

#include "identity_resolver.h"
#include <stdint.h>

typedef struct _NostrEvent NostrEvent;

typedef enum { MD_RELAY_CONFIGURED, MD_RELAY_NIP05, MD_RELAY_NPROFILE,
               MD_RELAY_NIP65, MD_RELAY_DM_LIST } MdRelaySource;
typedef enum { MD_RELAY_BOOTSTRAP, MD_RELAY_DISCOVERY, MD_RELAY_STORE,
               MD_RELAY_FETCH, MD_RELAY_DM_IN, MD_RELAY_DM_OUT } MdRelayRole;
typedef enum { MD_RELAY_READY, MD_RELAY_NO_BOOTSTRAP, MD_RELAY_NO_TARGETS } MdRelayPlanStatus;
#define MD_RELAY_CONNECTION_MAX 32

typedef struct { char *url; MdRelaySource source; } MdRelayTarget;
typedef struct {
    MdRelayTarget target[MD_RELAY_TARGET_MAX];
    size_t count;
    uint64_t deadline_ns; /* Optional shared monotonic deadline for DNS validation. */
} MdRelayPlan;
typedef struct { NostrEvent *nip65, *dm; } MdRelayMetadata;
typedef struct { const MdRelayTarget *target; unsigned mask; } MdRelayUse;

/* Round-robin a bounded connection union, merging roles for the same URL.
 * Returned targets borrow from plans. */
size_t md_relay_plan_union(const MdRelayPlan *plans, const unsigned *roles,
                           size_t count, MdRelayUse out[MD_RELAY_CONNECTION_MAX]);

/* Canonicalize, enforce WSS + public addresses for automatic sources, and
 * deduplicate. Explicitly configured URLs may be private or use WS. */
bool md_relay_plan_add(MdRelayPlan *plan, const char *url, MdRelaySource source);
/* Recheck immediately before pool admission. This is not the dial: DNS can
 * change between this check and nostrc's initial/reconnect resolution (md-dc2m). */
bool md_relay_target_check(const MdRelayTarget *target, uint64_t deadline_ns);
/* Seed an empty bootstrap plan for a single connection from public nprofile
 * hints, retaining discovered (not configured) provenance. */
bool md_relay_plan_profile_bootstrap(MdRelayPlan *plan, const MdIdentity *address);
void md_relay_plan_clear(MdRelayPlan *plan);
const char *md_relay_source_name(MdRelaySource source);

/* Keeps the NIP-01 replaceable winner for the expected author and kind.
 * The input event is borrowed; the metadata owns a deep copy on success. */
bool md_relay_metadata_offer(MdRelayMetadata *metadata, const char *author, const NostrEvent *event);
void md_relay_metadata_clear(MdRelayMetadata *metadata);
MdRelayPlanStatus md_relay_plan_build(MdRelayPlan *out, MdRelayRole role,
    const MdRelayPlan *bootstrap, const MdIdentity *address, const MdRelayMetadata *metadata);

typedef enum { MD_RELAY_AUTH_UNKNOWN, MD_RELAY_AUTH_NOT_ADVERTISED,
               MD_RELAY_AUTH_REQUIRED } MdRelayAuth;
MdRelayAuth md_relay_auth_from_json(const char *json);
MdRelayAuth md_relay_probe_auth(const MdRelayTarget *target);

#endif
