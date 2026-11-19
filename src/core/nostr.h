/*
 * metadesk — nostr.h
 * Thin bridge to nostrc library (github.com/chebizarro/nostrc).
 *
 * metadesk does NOT implement Nostr primitives. All key management,
 * encryption (NIP-44), DMs (NIP-17), allowlists (NIP-51), relay
 * connections, and event handling use the nostrc C library.
 *
 * PROTOCOL MODEL (NIP-01):
 *   Nostr is event-driven pub/sub over persistent WebSocket connections.
 *   Relay connections are stateful and last for the app's lifetime.
 *   Clients send REQ (subscribe) and EVENT (publish). Relays push
 *   matching events asynchronously, signal EOSE (end of stored events),
 *   and may send CLOSED. The nostrc NostrSimplePool manages connections,
 *   subscriptions, and message routing internally.
 *
 * This header provides a metadesk-specific façade that maps metadesk
 * session concepts to the underlying nostrc event-driven APIs.
 */
#ifndef MD_NOSTR_H
#define MD_NOSTR_H

#include "signer.h"
#include "accounts.h"
#include "session.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* nostrc public headers */
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-relay.h>
#include <nostr-simple-pool.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip51/nip51.h>
#include <nostr/nip17/nip17.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default signaling relay used by host/client when no
 * --relay option is given. Single definition to keep the binaries
 * in step. */
#define MD_DEFAULT_RELAY "wss://relay.sharegap.net"

/* ── Metadesk Nostr context ──────────────────────────────────
 * Wraps a nostrc relay pool + keypair + live subscriptions
 * for metadesk session operations.
 *
 * The pool maintains persistent WebSocket connections to all
 * configured relays. Subscriptions are registered at startup
 * and remain active for the lifetime of the MdNostr context.
 */
typedef struct MdNostr MdNostr;

typedef enum {
    MD_NOSTR_ACL_UNKNOWN = 0, /* history pending: deny */
    MD_NOSTR_ACL_LOADED,      /* history complete; empty means deny all */
    MD_NOSTR_ACL_FAILED,      /* no usable history: deny */
    MD_NOSTR_ACL_OPEN,        /* explicitly selected by configuration */
} MdNostrAclState;

/* Configuration for the Nostr bridge.
 *
 * Authentication: provide EITHER signer OR sk_hex (not both).
 *   - signer:  MdSigner* from any backend (NIP-46/55L/5F/direct-key).
 *              MdNostr retains a reference; caller retains its own ownership.
 *   - sk_hex:  64-char hex secret key (legacy). MdNostr creates an
 *              internal direct-key signer and takes ownership of it.
 *
 * If both are provided, signer takes precedence and sk_hex is ignored.
 */
typedef struct {
    MdSigner    *signer;          /* pluggable signer (preferred)      */
    const char  *sk_hex;          /* 64-char hex secret key (fallback) */
    const char **relay_urls;      /* NULL-terminated array of relay URLs */
    int          relay_count;
    /* Optional checked owner. Retained until destroy; signer must be NULL or
     * the binding's signer. Revocation stops this bridge, never the signer. */
    MdAccountBinding *owner;
    bool allowlist_open; /* explicit opt-out of ACL enforcement; default false */
} MdNostrConfig;

/* ── Event callbacks ──────────────────────────────────────────
 * Nostr is event-driven. These callbacks are invoked asynchronously
 * when matching events arrive on live subscriptions.
 */

/* Called when a session-related DM (NIP-17 gift-wrap) is received and decrypted */
/* Runs synchronously on the control thread revoking the role, after bridge
 * admission/cancellable operations stop. Must not destroy the bridge here. */
typedef void (*MdNostrOwnerRevokedCallback)(void *userdata);

/* Delivery facts for one validated DM. NIP-59 randomizes the seal and
 * gift-wrap timestamps, never the rumor's: rumor_created_at is the sender's
 * authenticated send time. `live` is true only when the gift wrap arrived
 * after its relay subscription's EOSE, in that subscription's ordered
 * delivery (catch-up -> live boundary); stored history and post-reconnect
 * catch-up are not live. Real-time commands such as session requests must
 * not be answered from history. */
typedef struct {
    int64_t rumor_created_at;
    bool    live;
} MdNostrDmInfo;

typedef void (*MdNostrDmCallback)(const char *sender_pubkey_hex,
                                  const char *content,
                                  const MdNostrDmInfo *info,
                                  void *userdata);


/* Called when a relay responds to an event we published (OK frame).
 * `ok` is true on acceptance, false on rejection.
 * `reason` may be NULL; common values include "blocked:", "duplicate:",
 * "pow:", "rate-limited:", "invalid:", "error:" (NIP-20 machine-readable). */
typedef void (*MdNostrPublishResultCallback)(const char *event_id,
                                             bool ok,
                                             const char *reason,
                                             void *userdata);

/* Bounded inbound DM pipeline. These limits cover serialized gift-wraps,
 * queued wraps (one worker), and the recent wrap/rumor replay windows. */
#define MD_NOSTR_DM_MAX_BYTES 131072
#define MD_NOSTR_DM_QUEUE_CAPACITY 32
#define MD_NOSTR_DM_DEDUP_CAPACITY 256

typedef enum {
    MD_NOSTR_DM_OK = 0,
    MD_NOSTR_DM_INVALID_WRAP,
    MD_NOSTR_DM_WRONG_RECIPIENT,
    MD_NOSTR_DM_TOO_LARGE,
    MD_NOSTR_DM_DECRYPT_FAILED,
    MD_NOSTR_DM_INVALID_SEAL,
    MD_NOSTR_DM_SENDER_MISMATCH,
    MD_NOSTR_DM_INVALID_RUMOR,
    MD_NOSTR_DM_DUPLICATE,
    MD_NOSTR_DM_QUEUE_FULL,
    MD_NOSTR_DM_STOPPED,
    MD_NOSTR_DM_NO_MEMORY,
} MdNostrDmResult;

const char *md_nostr_dm_result_name(MdNostrDmResult result);

/* Callback table registered at creation time */
typedef struct {
    MdNostrDmCallback              on_dm;              /* incoming session DMs     */
    void                          *dm_userdata;
    MdNostrPublishResultCallback   on_publish_result;  /* relay OK/rejection       */
    void                          *publish_result_userdata;
    /* Worker validation failures, never plaintext/key material. */
    void (*on_dm_error)(MdNostrDmResult result, void *userdata);
    MdNostrOwnerRevokedCallback on_owner_revoked;
    void *owner_userdata;
} MdNostrCallbacks;

/* ── Lifecycle ────────────────────────────────────────────────
 * Create sets up the relay pool, connects to all relays, and
 * if an on_dm callback is provided, subscribes to kind:1059
 * NIP-17 gift-wraps addressed to our pubkey.
 *
 * Additional subscriptions are started on demand:
 *   - kind:30000 via md_nostr_refresh_allowlist()
 *
 * All subscriptions persist until md_nostr_destroy().
 */

/* Create Nostr bridge. Connects to relays, probes NIP-11 relay info,
 * derives pubkey, subscribes to kind:1059 gift-wraps if on_dm is set. */
MdNostr *md_nostr_create(const MdNostrConfig *cfg, const MdNostrCallbacks *cbs);

/* Get the local pubkey hex string (owned by MdNostr, do not free). */
const char *md_nostr_get_npub(const MdNostr *n);

/* Get the signer used by this Nostr bridge (borrowed reference). */
MdSigner *md_nostr_get_signer(MdNostr *n);

/* Stop external producers before destroy; no new calls may start afterward.
 * Invalidates pending DM results and joins accepted operations before
 * freeing callbacks/state. An already-admitted callback may finish.
 * Never call destroy from a bridge callback (it joins the callback worker).
 * A shared signer is retained, not shut down, by the bridge. */
void md_nostr_destroy(MdNostr *n);

/* Invalidate inbound/outbound admission immediately without freeing state.
 * Pending DM results are discarded; destroy joins any non-abortable signer
 * call. Does not shut down a signer shared with another bridge/role. */
void md_nostr_stop(MdNostr *n);

/* Bounded ingress used by relay middleware; copies the event on acceptance.
 * Duplicate delivery is suppressed within bounded recent-ID windows, including
 * rewrapping the same rumor. Queue-full is explicit backpressure, not a retry
 * timer. Callers may use drain to await accepted work, not relay history. */
MdNostrDmResult md_nostr_receive_dm(MdNostr *n, NostrEvent *gift_wrap);
void md_nostr_drain_dm(MdNostr *n);

/* Wait until every relay's subscription has reached its catch-up -> live
 * boundary (EOSE) or a terminal state. Returns 0 if at least one relay
 * delivered EOSE, -1 on timeout, all-failed or stop. A readiness bound only:
 * subscriptions stay open either way. */
int md_nostr_wait_history(MdNostr *n, uint32_t timeout_ms);

/* Deterministic lifecycle diagnostics for tests. Configure only while idle;
 * hooks run outside bridge locks and must not re-enter bridge APIs. NULL is
 * the production default. ADMITTED may block to hold an in-flight reader. */
typedef enum {
    MD_NOSTR_TEST_ADMITTED, MD_NOSTR_TEST_SERIALIZING, MD_NOSTR_TEST_DRAINING,
    MD_NOSTR_TEST_ALLOWLIST_RECEIVED, MD_NOSTR_TEST_ALLOWLIST_BOUNDARY,
    MD_NOSTR_TEST_ALLOWLIST_SIGNED, MD_NOSTR_TEST_AUTH_RESULT,
    MD_NOSTR_TEST_ALLOWLIST_PROCESSING
} MdNostrTestPoint;
void md_nostr_set_test_hook(MdNostr *n,
    void (*hook)(MdNostrTestPoint, void *), void *userdata);

/* ── Session signaling (NIP-17 gift-wrapped DMs) ──────────────
 * Session messages are sent as NIP-17 gift-wrapped DMs:
 *   rumor (kind:14) → seal (kind:13) → gift-wrap (kind:1059)
 * The recipient's live kind:1059 subscription picks them up
 * asynchronously; the on_dm callback fires with decrypted content.
 */

/* Send a session request to host_pubkey_hex. Returns 0 on publish success. */
int md_nostr_send_session_request(MdNostr *n, const char *host_pubkey_hex,
                                  const char *json_payload);

/* Send a session accept to client_pubkey_hex. Returns 0 on publish success. */
int md_nostr_send_session_accept(MdNostr *n, const char *client_pubkey_hex,
                                 const char *json_payload);

/* ── Access control (NIP-51 allowlists) ───────────────────────
 * The host maintains a kind:30000 addressable event (NIP-51 list)
 * with d-tag "metadesk-allowlist" containing authorised client pubkeys.
 * This is a replaceable event — only the latest version matters.
 */

/* Membership only, NOT capability authorization. */
bool md_nostr_is_allowed(MdNostr *n, const char *pubkey_hex);

/* Atomic requested & entry grant from the loaded policy. Missing/legacy/
 * malformed/duplicate scopes deny; UNKNOWN/FAILED deny. Explicit OPEN permits
 * known bits only. Recheck at session acceptance; zero means refuse. */
uint32_t md_nostr_granted_capabilities(MdNostr *n, const char *pubkey_hex,
                                      uint32_t requested);

/* The same grant, plus whether it is the policy's answer: *known (if
 * non-NULL) is set, under the same lock, to false while readiness is UNKNOWN
 * or FAILED (the zero means "not known yet", not "denied") and to true for
 * LOADED/OPEN, an invalid key or non-host bridge, and once stopped or revoked
 * (every grant then denies for good). */
uint32_t md_nostr_granted_capabilities_known(MdNostr *n, const char *pubkey_hex,
                                            uint32_t requested, bool *known);

/* Strict CLI vocabulary: none or a canonical subset of video,agent,input.
 * Returns -1 for unknown/duplicate/out-of-order/empty tokens; clears *out. */
int md_nostr_parse_caps(const char *text, uint32_t *out);

/* History readiness is independent of membership. UNKNOWN/FAILED deny, and
 * LOADED with zero entries denies all. OPEN is only selected by config.
 * Authorize with md_nostr_granted_capabilities(), never has_allowlist(). */
MdNostrAclState md_nostr_allowlist_state(MdNostr *n);

/* Block until the readiness state or the accepted-list version differs from
 * the caller's state and version (start from UNKNOWN and 0), then store the
 * current pair.
 * Wakes on EOSE/CLOSED/connection boundaries and on each accepted newer
 * replaceable list; never a timer. Returns -1 once stopped/revoked.
 * Observability only: authorize with md_nostr_granted_capabilities(). */
int md_nostr_allowlist_wait_change(MdNostr *n, MdNostrAclState *state,
                                   uint64_t *version);

/* True when history has loaded (including an empty list); not an auth check. */
bool md_nostr_has_allowlist(const MdNostr *n);

/* Subscribe to allowlist updates from relays (limit:1 for latest). */
int md_nostr_refresh_allowlist(MdNostr *n);

/* Stage an idempotent membership edit, sign and wait for at least one OK=true,
 * then commit only if the base ACL version is unchanged. Pending history waits
 * for EOSE/failure or stop/revocation. Returns -1 on failed history, signer/
 * publish failure, a busy edit, or concurrent replacement.
 * A conflict can already have reached a relay; refresh/reconcile before retry.
 * NULL caps preserves an existing member or adds a new video-only member.
 * Explicit canonical caps upserts membership and a v1 grant (none revokes all).
 * Removal drops every matching membership and grant. Identical edits no-op. */
int md_nostr_allowlist_add(MdNostr *n, const char *pubkey_hex, const char *caps);

/* Remove pubkey_hex from allowlist, publish updated list. */
int md_nostr_allowlist_remove(MdNostr *n, const char *pubkey_hex);

/* Allowlist entry snapshot (copied out under the allowlist lock) */
typedef struct {
    char pubkey_hex[65];  /* NUL-terminated npub hex (64 chars)  */
    char caps[64];        /* effective canonical caps, or "none" */
} MdAllowlistEntry;

/* Wait for EOSE/CLOSED/connection failure (or stop/revocation), then copy a
 * coherent membership snapshot. Returns -1 unless history loaded successfully.
 * Empty history succeeds with *entries=NULL and *count=0. Caller frees entries.
 * No deadline is interpreted as successful history completion. */
int md_nostr_allowlist_snapshot(MdNostr *n, MdAllowlistEntry **entries, size_t *count);

/* Return number of entries on the current allowlist (0 if none). */
int md_nostr_allowlist_count(const MdNostr *n);

/*
 * Copy entry at index into *out. Returns 0 on success, -1 if out of range.
 * The copy remains valid independent of later allowlist mutations.
 */
int md_nostr_allowlist_get_entry(const MdNostr *n, int index,
                                 MdAllowlistEntry *out);


/* ── Generic event publishing ─────────────────────────────── */

/*
 * Sign an event through the signer abstraction and return the signed
 * event JSON. Universal signing path shared by all event builders
 * (DM seals, allowlists, session log entries).
 * Returns 0 on success (caller frees *out_json), -1 on error.
 */
int md_nostr_sign_event_json(MdSigner *signer, NostrEvent *ev,
                             char **out_json);

/*
 * Publish an already-signed event (JSON string) to all connected relays.
 * The JSON must contain a valid signed Nostr event with id, pubkey, sig.
 * Returns 0 on success, -1 on error.
 *
 * Use this to publish arbitrary signed events (e.g. session log entries)
 * without going through the kind-specific helpers above.
 */
int md_nostr_publish_signed_json(MdNostr *n, const char *signed_event_json);

/* ── Key utilities (delegates to nostrc) ──────────────────── */

/* Generate a new random Nostr keypair. Caller frees both strings. */
int md_nostr_generate_keypair(char **sk_hex_out, char **pk_hex_out);

/* Get public key hex from secret key hex. Caller frees.
 * Note: prefer md_signer_get_pubkey() for signer-based auth. */
char *md_nostr_get_pubkey(const char *sk_hex);

#ifdef __cplusplus
}
#endif

#endif /* MD_NOSTR_H */
