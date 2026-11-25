#ifndef MD_MANAGED_STORE_H
#define MD_MANAGED_STORE_H

#include "accounts.h"
#include <nostr-event.h>

#define MD_STORE_HOSTS "metadesk-hosts"
#define MD_STORE_FAVORITES "metadesk-favorites"
#define MD_STORE_TAG_PREFIX "metadesk-tag:"
#define MD_STORE_MANAGEMENT "metadesk:management:v1"
#define MD_STORE_MAX_CONTENT (256u * 1024u)
/* Wire/schema ceilings; the pinned NIP-44 v2 signer has a tighter plaintext bound. */
#define MD_STORE_MAX_PLAINTEXT 65535u
#define MD_STORE_MAX_HOSTS 1000
#define MD_STORE_MAX_RELAYS 32

typedef struct MdManagedStore MdManagedStore;
/* Malloc-owned platform default. Computes a path, never creates it. */
char *md_managed_store_default_directory(void);
typedef enum {
    MD_STORE_OK, MD_STORE_UNCHANGED, MD_STORE_PENDING, MD_STORE_PARTIAL,
    MD_STORE_CONFLICT, MD_STORE_READ_ONLY, MD_STORE_NOT_READY,
    MD_STORE_INVALID, MD_STORE_SIGNER, MD_STORE_IO, MD_STORE_REVOKED,
    MD_STORE_CLOCK, MD_STORE_LIMIT
} MdStoreResult;

/* Serialize calls, including relay callbacks. state_directory is an existing
 * private application directory. The store holds an exclusive owner lock. */
MdManagedStore *md_managed_store_open(MdAccountBinding *client, const char *state_directory);
void md_managed_store_close(MdManagedStore *);

/* Caller runs the live REQ on its chosen routes, feeds EVENT, and marks
 * completion after its required EOSE barrier. AUTH/CLOSED or reconnect reset
 * the barrier; retain cache throughout. Do not CLOSE at EOSE. Do not mark
 * completion after an admission I/O failure until replay has repaired it. */
char *md_managed_store_filter(MdManagedStore *, const char *coordinate);
MdStoreResult md_managed_store_backfill(MdManagedStore *, const char *coordinate, bool complete);
MdStoreResult md_managed_store_receive(MdManagedStore *, const char *coordinate,
                                      const char *signed_event_json, int64_t now);

/* Winning revision (empty means no event) and optional decrypted winner and
 * separate draft. Caller owns plaintext strings and must not persist them.
 * READ_ONLY takes precedence over draft status. A schema-invalid winner
 * returns its revision, without plaintext. Signer failures are separate. */
MdStoreResult md_managed_store_read(MdManagedStore *, const char *coordinate,
    char revision[65], char **plaintext, char **pending_plaintext);
/* Complete replacement from base after backfill; durable before send.
 * Targets are caller-selected relay URLs; only normalized hashes persist. */
MdStoreResult md_managed_store_stage(MdManagedStore *, const char *coordinate,
    const char *base, const char *plaintext, const char *const *targets, size_t count, int64_t now);
/* Retry returns the same signed EVENT, for an unacknowledged target only.
 * Before handing a draft to transport, durably marks it possibly sent.
 * Check the same role generation before sending; use cancellable relay I/O.
 * Transport correlates OK by relay and event ID. Negative/unknown outcomes
 * never commit; transport owns NIP-42 and reconnect handling. */
MdStoreResult md_managed_store_outgoing(MdManagedStore *, const char *coordinate,
    const char *target, char **signed_event_json);
typedef enum {
    MD_STORE_RELAY_WAITING, MD_STORE_RELAY_REJECTED, MD_STORE_RELAY_AUTH_REQUIRED,
    MD_STORE_RELAY_CLOSED, MD_STORE_RELAY_UNKNOWN, MD_STORE_RELAY_ACKNOWLEDGED
} MdStoreRelayResult;
MdStoreResult md_managed_store_record_result(MdManagedStore *, const char *coordinate,
    const char *target, const char *event_id, MdStoreRelayResult);
/* Receipt retained while that signed event still has outstanding targets. */
MdStoreResult md_managed_store_relay_result(MdManagedStore *, const char *coordinate,
    const char *target, MdStoreRelayResult *);
/* Conflict actions. Reapply requires the currently winning base and replaces
 * the previous draft atomically only after validating and signing the edit. */
/* Republish the exact remote content to the original write targets if the
 * discarded draft may have escaped. Never-sent drafts can be dropped. */
MdStoreResult md_managed_store_keep_remote(MdManagedStore *, const char *coordinate,
    const char *base, int64_t now);
MdStoreResult md_managed_store_reapply(MdManagedStore *, const char *coordinate,
    const char *base, const char *reviewed_plaintext, const char *const *targets, size_t count, int64_t now);
/* Explicit recovery: replace a decrypted but invalid winner at its exact
 * revision. Does not recover missing/decryption-failed/future-version data. */
MdStoreResult md_managed_store_replace_invalid(MdManagedStore *, const char *coordinate,
    const char *base, const char *replacement, const char *const *targets, size_t count, int64_t now);

/* Local safety switch; missing/unreadable is disabled. It is not authority:
 * host policy must also be synchronized and explicitly enabled by the user. */
MdStoreResult md_managed_store_set_unattended(MdManagedStore *, bool enabled);
bool md_managed_store_unattended(MdManagedStore *);

typedef enum { MD_RECENT_CONNECTED, MD_RECENT_RESOLVE, MD_RECENT_RELAY, MD_RECENT_SIGNER,
               MD_RECENT_NEGOTIATION, MD_RECENT_FIPS, MD_RECENT_BIND, MD_RECENT_MEDIA } MdRecentOutcome;
MdStoreResult md_managed_store_record(MdManagedStore *, const char *pubkey,
                                     int64_t at, MdRecentOutcome);
char *md_managed_store_recents(MdManagedStore *);

/* Explicit plaintext exchange only; never opens a file. Import returns
 * candidate array JSON for TOFU review; export projects only format fields. */
MdStoreResult md_managed_store_import(const char *text, char **candidates);
MdStoreResult md_managed_store_export(const char *candidates, char **text);

/* Scrub and release an exclusively owned decrypted JSON tree. Do not call
 * while any node has aliases still in use. */
struct json_t;
void md_managed_store_discard_plaintext(struct json_t *);

/* Offline, read-only projection of committed cache, even with a writer open.
 * Returns an owned array of saved hosts with metadata, favorites first then
 * newest recents, deduplicated. NULL means unavailable, not an empty book.
 * Release with md_managed_store_discard_plaintext(). */
struct json_t *md_managed_store_quick_hosts(MdAccountBinding *, const char *state_directory);
/* The optional operation token must outlive the read. Revoking the binding
 * also cancels it. Already-running non-abortable signer calls may drain. */
struct json_t *md_managed_store_quick_hosts_cancellable(MdAccountBinding *,
    const char *state_directory, MdSignerCancel *);

/* Shared with existing signer-backed ACL and quality writers. */
NostrEvent *md_private_document_build(MdSigner *, const MdSignerCancel *,
    const char *author, int kind, const char *coordinate, const char *plaintext, int64_t at);
#endif
