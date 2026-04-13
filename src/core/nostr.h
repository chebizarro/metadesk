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

/* ── Metadesk Nostr context ──────────────────────────────────
 * Wraps a nostrc relay pool + keypair + live subscriptions
 * for metadesk session operations.
 *
 * The pool maintains persistent WebSocket connections to all
 * configured relays. Subscriptions are registered at startup
 * and remain active for the lifetime of the MdNostr context.
 */
typedef struct MdNostr MdNostr;

/* Configuration for the Nostr bridge.
 *
 * Authentication: provide EITHER signer OR sk_hex (not both).
 *   - signer:  MdSigner* from any backend (NIP-46/55L/5F/direct-key).
 *              MdNostr borrows the pointer — caller retains ownership.
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
} MdNostrConfig;

/* ── Event callbacks ──────────────────────────────────────────
 * Nostr is event-driven. These callbacks are invoked asynchronously
 * when matching events arrive on live subscriptions.
 */

/* Called when a session-related DM (NIP-17 gift-wrap) is received and decrypted */
typedef void (*MdNostrDmCallback)(const char *sender_pubkey_hex,
                                  const char *content,
                                  void *userdata);

/* Called when a transport address event (kind:30078) is received */
typedef void (*MdNostrTransportCallback)(const char *pubkey_hex,
                                         const char *fips_addr,
                                         void *userdata);

/* Callback table registered at creation time */
typedef struct {
    MdNostrDmCallback         on_dm;           /* incoming session DMs     */
    void                     *dm_userdata;
    MdNostrTransportCallback  on_transport;    /* transport addr updates   */
    void                     *transport_userdata;
} MdNostrCallbacks;

/* ── Lifecycle ────────────────────────────────────────────────
 * Create sets up the relay pool, connects to all relays, and
 * registers live subscriptions for:
 *   - kind:1059 (NIP-17 gift-wraps addressed to our pubkey)
 *   - kind:30078 (FIPS transport address publications)
 * These subscriptions persist until md_nostr_destroy().
 */

/* Create Nostr bridge. Connects to relays, derives pubkey, starts subscriptions. */
MdNostr *md_nostr_create(const MdNostrConfig *cfg, const MdNostrCallbacks *cbs);

/* Get the local pubkey hex string (owned by MdNostr, do not free). */
const char *md_nostr_get_npub(const MdNostr *n);

/* Get the signer used by this Nostr bridge (borrowed reference). */
MdSigner *md_nostr_get_signer(MdNostr *n);

/* Destroy Nostr bridge: close subscriptions, disconnect relays, zero key material. */
void md_nostr_destroy(MdNostr *n);

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

/* Check if pubkey_hex is on the cached allowlist. */
bool md_nostr_is_allowed(MdNostr *n, const char *pubkey_hex);

/* Refresh the allowlist from relays (subscribe, wait for EOSE). */
int md_nostr_refresh_allowlist(MdNostr *n);

/* Add pubkey_hex to allowlist with capabilities, publish updated list. */
int md_nostr_allowlist_add(MdNostr *n, const char *pubkey_hex, const char *caps);

/* Remove pubkey_hex from allowlist, publish updated list. */
int md_nostr_allowlist_remove(MdNostr *n, const char *pubkey_hex);

/* ── Transport address publication ────────────────────────────
 * The host publishes its FIPS transport address as a kind:30078
 * addressable event with d-tag "fips-transport". Clients subscribe
 * to this event to discover the host's address. Updates arrive
 * asynchronously via the on_transport callback.
 */

/* Publish our FIPS transport address (kind:30078, d:"fips-transport"). */
int md_nostr_publish_transport(MdNostr *n, const char *fips_addr);

/* Subscribe to a specific host's transport address updates.
 * Results arrive asynchronously via the on_transport callback. */
int md_nostr_subscribe_transport(MdNostr *n, const char *host_pubkey_hex);

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
