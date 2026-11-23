/*
 * metadesk — session_log.h
 * Signed Nostr session event log for agent monitoring mode (Spec M2.4).
 *
 * Records session lifecycle events (connect, disconnect, action, etc.)
 * as signed Nostr events (kind:1078). Each entry is signed with the
 * host's signer for non-repudiation. Relay publication is off by default;
 * opting in publishes separate encrypted copies, never the local events.
 *
 * The log maintains a fixed-size ring buffer of recent entries in memory.
 * Oldest entries are overwritten when the buffer is full.
 *
 * Thread safety: callers must serialise calls to md_session_log_event().
 */
#ifndef MD_SESSION_LOG_H
#define MD_SESSION_LOG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations — avoids pulling in full headers */
typedef struct MdSigner MdSigner;
typedef struct MdNostr  MdNostr;

/* ── Event types ─────────────────────────────────────────────── */

typedef enum {
    MD_SESSION_LOG_CONNECT,      /* client connected (TCP or FIPS)     */
    MD_SESSION_LOG_DISCONNECT,   /* client disconnected                */
    MD_SESSION_LOG_ACTION,       /* agent action performed             */
    MD_SESSION_LOG_REQUEST,      /* session request received           */
    MD_SESSION_LOG_ACCEPT,       /* session request accepted           */
    MD_SESSION_LOG_DENY,         /* session request denied             */
} MdSessionLogEventType;

/* Human-readable name for an event type */
const char *md_session_log_event_name(MdSessionLogEventType type);

/* ── Log entry ───────────────────────────────────────────────── */

typedef struct {
    MdSessionLogEventType type;
    char    session_id[64];     /* session UUID (may be empty)          */
    char    peer_pubkey[128];   /* peer's pubkey hex (may be empty)     */
    char    detail[256];        /* human-readable detail string         */
    int64_t timestamp;          /* Unix timestamp (seconds)             */
    char   *signed_json;        /* signed Nostr event JSON, or NULL     */
} MdSessionLogEntry;

/* ── Session log context ─────────────────────────────────────── */

typedef struct MdSessionLog MdSessionLog;

/* Local signed record and app-specific NIP-44 encrypted relay copy. */
#define MD_SESSION_LOG_KIND 1078
#define MD_SESSION_LOG_RELAY_KIND 1079

/* Default ring buffer capacity */
#define MD_SESSION_LOG_DEFAULT_CAP 128

typedef struct {
    MdSigner *signer;    /* for signing entries (borrowed, may be NULL)  */
    MdNostr  *nostr;     /* for publishing entries (borrowed, may be NULL) */
    bool      publish;   /* if true and nostr != NULL, publish to relays */
    int       capacity;  /* ring buffer size (0 = default 128)           */
} MdSessionLogConfig;

/* ── Lifecycle ───────────────────────────────────────────────── */

/*
 * Create a session log. Config fields are all optional:
 *   - signer: enables signing entries as Nostr events
 *   - nostr + publish: enables publishing to relays
 *   - capacity: overrides default ring buffer size
 * Returns NULL on allocation failure.
 */
MdSessionLog *md_session_log_create(const MdSessionLogConfig *cfg);

/* Destroy session log, freeing all entries. */
void md_session_log_destroy(MdSessionLog *log);

/* Attach a bridge after deferred initialization. NULL detaches, discards
 * pending copies and joins the worker. Lifecycle calls must be serialized;
 * stop and detach a borrowed bridge before destroying it. */
void md_session_log_set_nostr(MdSessionLog *log, MdNostr *nostr);

/* ── Logging ─────────────────────────────────────────────────── */

/*
 * Record a session event.
 *   type:        event type (connect, disconnect, action, etc.)
 *   session_id:  session UUID string (may be NULL)
 *   peer_pubkey: peer's pubkey hex (may be NULL)
 *   detail:      human-readable detail (may be NULL)
 *
 * If a signer is configured, the entry is signed as a kind:1078 event.
 * If opted in and attached, enqueue a best-effort encrypted relay copy.
 * The caller never waits for relay delivery; the local signing result is
 * independent of relay delivery.
 *
 * Returns 0 on success, -1 on error.
 */
int md_session_log_event(MdSessionLog *log, MdSessionLogEventType type,
                         const char *session_id, const char *peer_pubkey,
                         const char *detail);

/* ── Query ───────────────────────────────────────────────────── */

/* Number of entries currently in the log (up to capacity). */
int md_session_log_count(const MdSessionLog *log);

/*
 * Get a log entry by index (0 = oldest still in buffer).
 * Returns pointer to internal entry (valid until next log_event call
 * that overwrites it), or NULL if index is out of range.
 */
const MdSessionLogEntry *md_session_log_get(const MdSessionLog *log, int index);

/*
 * Build the unsigned event content JSON for a log entry.
 * Returns a heap-allocated JSON string. Caller frees.
 * Returns NULL on error.
 *
 * Exposed for testing; normally called internally by md_session_log_event().
 */
char *md_session_log_build_content(MdSessionLogEventType type,
                                   const char *session_id,
                                   const char *peer_pubkey,
                                   const char *detail,
                                   int64_t timestamp);

#ifdef __cplusplus
}
#endif

#endif /* MD_SESSION_LOG_H */
