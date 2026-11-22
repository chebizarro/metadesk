/*
 * metadesk — session.h
 * Session state machine and lifecycle management.
 * See spec §4 for session negotiation flow.
 */
#ifndef MD_SESSION_H
#define MD_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Session states — spec §4.1 flow */
typedef enum {
    MD_SESSION_IDLE,
    MD_SESSION_REQUESTING,
    MD_SESSION_NEGOTIATING,
    MD_SESSION_ACTIVE,
    MD_SESSION_DISCONNECTING,
} MdSessionState;

/* Client capabilities — bitfield */
typedef enum {
    MD_CAP_VIDEO = (1 << 0),
    MD_CAP_AGENT = (1 << 1),
    MD_CAP_INPUT = (1 << 2),
} MdCapability;

/* Tree format preference */
typedef enum {
    MD_TREE_FORMAT_JSON,
    MD_TREE_FORMAT_COMPACT,
} MdTreeFormat;

/* Default keepalive interval in milliseconds */
#define MD_SESSION_KEEPALIVE_DEFAULT_MS 5000

/* Timeout multiplier: session is dead after this many missed keepalives */
#define MD_SESSION_KEEPALIVE_TIMEOUT_MULT 3

/* Session context */
typedef struct {
    MdSessionState state;
    uint32_t       capabilities;   /* bitwise OR of MdCapability   */
    MdTreeFormat   tree_format;
    char           session_id[64]; /* UUID string                  */
    char           peer_npub[128]; /* remote npub                  */
    uint32_t       keepalive_ms;   /* keepalive interval            */
    uint64_t       last_ping_ms;   /* timestamp of last ping sent   */
    uint64_t       last_pong_ms;   /* timestamp of last pong recv   */
    /* Host-only, ephemeral one-use stream reservation. Never log/persist. */
    char           stream_token[65];
    uint64_t       stream_deadline_ms;
} MdSession;

/* Initialise session to IDLE state */
void md_session_init(MdSession *s);

/* Request a session with a remote host npub */
int md_session_request(MdSession *s, const char *peer_npub, uint32_t caps,
                       MdTreeFormat tree_fmt);

/* Accept an incoming session (host side) */
int md_session_accept(MdSession *s, const char *session_id, uint32_t granted_caps);

/* Transition to ACTIVE after handshake completes */
int md_session_activate(MdSession *s);

/* Initiate graceful disconnect */
int md_session_disconnect(MdSession *s);

/* Reset/reinitialize a session back to IDLE after disconnect or failure. */
void md_session_reset(MdSession *s);

/* Process a keepalive ping; returns true if pong should be sent */
bool md_session_on_ping(MdSession *s, uint32_t timestamp_ms);

/* Record a pong response */
void md_session_on_pong(MdSession *s, uint32_t timestamp_ms);

/* Check if the session has timed out (missed keepalives) */
bool md_session_is_timed_out(const MdSession *s, uint64_t now_ms);

/* ── Session signaling JSON payloads (spec §4, Phase 2.1) ──────
 *
 * These are the NIP-17 DM content payloads exchanged during session
 * negotiation, and the MD_PKT_SESSION_INFO wire payloads.
 *
 * Request (client → host DM):
 *   {"type":"session_request","v":1,"request_id":"<32 hex>",
 *    "issued_at":<unix s>,"expires_at":<unix s>,
 *    "stream_tls":"tls13-sha256","capabilities":["video","agent","input"],
 *    "tree_format":"compact","fips_addr":"npub1..."}
 *
 * Accept (host → client DM):
 *   {"type":"session_accept","v":1,"request_id":"<32 hex>",
 *    "session_id":"<uuid>","stream_token":"<64 hex>",
 *    "tls_cert_sha256":"<64 lowercase hex>",
 *    "granted":["video","agent"],"tree_format":"compact"}
 *
 * request_id/issued_at/expires_at are the request-correlation extension
 * (md-6ns, docs/stream-binding.md). Names and shapes match session-v2
 * (P3.8a) so that version can adopt them unchanged.
 */

/* Request correlation (md-6ns). A random 128-bit id per negotiation, as
 * 32 lowercase hex characters, bounded by authenticated wall-clock times. */
#define MD_SESSION_REQUEST_ID_LEN 32
#define MD_SESSION_REQUEST_MAX_LIFETIME_S 180
#define MD_SESSION_CLOCK_SKEW_S 30

/* Parsed session request from client */
typedef struct {
    uint32_t    capabilities;      /* bitwise OR of MdCapability */
    MdTreeFormat tree_format;
    bool         stream_tls;       /* supports pinned TLS 1.3 */
    char         fips_addr[128];   /* client's npub / FIPS addr  */
    char         request_id[MD_SESSION_REQUEST_ID_LEN + 1]; /* "" if absent */
    int64_t      issued_at;        /* unix seconds; 0 if absent  */
    int64_t      expires_at;       /* unix seconds; 0 if absent  */
} MdSessionRequest;

/* Parsed session accept from host */
typedef struct {
    char         session_id[64];   /* UUID assigned by host      */
    uint32_t     granted;          /* bitwise OR of MdCapability */
    MdTreeFormat tree_format;      /* confirmed tree format       */
    char         stream_token[65]; /* DM-only 256-bit binding secret */
    char         tls_cert_sha256[65]; /* DM-only lowercase DER certificate digest */
    char         request_id[MD_SESSION_REQUEST_ID_LEN + 1]; /* echoed; "" if absent */
} MdSessionAccept;

/* Authenticated host refusal: no session, token, or capabilities are granted.
 * Unknown reasons fail closed; display only the local, actionable text below. */
typedef struct {
    char request_id[MD_SESSION_REQUEST_ID_LEN + 1];
    char reason[32]; /* capture_unavailable, request_refused or tls_required */
} MdSessionReject;
char *md_session_reject_to_json(const MdSessionReject *reject);
int md_session_reject_from_json(const char *json, MdSessionReject *out);
bool md_session_reject_matches_request(const MdSessionReject *reject,
                                       const MdSessionRequest *req,
                                       int64_t rumor_created_at);
const char *md_session_reject_message(const MdSessionReject *reject);

/* Serialize a session request to a newly-allocated JSON string. Caller frees. */
char *md_session_request_to_json(const MdSessionRequest *req);

/* Parse a session request from JSON. Returns 0 on success, -1 on error. */
int md_session_request_from_json(const char *json, MdSessionRequest *out);

/* Serialize a session accept to a newly-allocated JSON string. Caller frees. */
char *md_session_accept_to_json(const MdSessionAccept *acc);

/* Parse a session accept from JSON. Returns 0 on success, -1 on error. */
int md_session_accept_from_json(const char *json, MdSessionAccept *out);

/* Transitional binding extension to v1 signaling (not full session-v2).
 * SESSION_INFO must carry session_bind as the first stream packet.
 * The token is delivered only in the authenticated acceptance DM. */
#define MD_SESSION_JSON_MAX 8192
#define MD_SESSION_TOKEN_LIFETIME_MS 30000
#define MD_SESSION_BIND_TIMEOUT_MS 5000

/* Reserve a CSPRNG token for a negotiating host session and fill its DM. */
int md_session_prepare_binding(MdSession *s, MdSessionAccept *acc, uint64_t now_ms);
char *md_session_bind_to_json(const MdSessionAccept *acc);
/* Validate bounded wire JSON, consume once and activate. Caller serializes
 * access to the reservation. Failure never activates or grants capabilities. */
int md_session_consume_binding(MdSession *s, const uint8_t *payload, size_t len,
                               uint64_t now_ms);

/* ── Request correlation (md-6ns) ──────────────────────────────
 * NIP-59 randomizes gift-wrap timestamps up to two days in the past, so a
 * relay `since` filter cannot separate a live negotiation from history.
 * Each negotiation instead carries a fresh CSPRNG request_id and an
 * authenticated validity window inside the encrypted rumor. */

/* Fill a fresh request_id and issued_at/expires_at window for `req`,
 * and advertise support for pinned TLS 1.3.
 * lifetime_s is clamped to [1, MD_SESSION_REQUEST_MAX_LIFETIME_S].
 * Returns -1 (and leaves no id) if the CSPRNG fails; never falls back. */
int md_session_request_prepare(MdSessionRequest *req, int64_t now_unix,
                               uint32_t lifetime_s);

typedef enum {
    MD_SESSION_REQUEST_FRESH = 0,
    MD_SESSION_REQUEST_UNCORRELATED, /* no request_id/window (legacy client) */
    MD_SESSION_REQUEST_BAD_WINDOW,   /* expires <= issued or lifetime > max  */
    MD_SESSION_REQUEST_FUTURE,       /* issued beyond the allowed clock skew */
    MD_SESSION_REQUEST_EXPIRED,      /* past expires_at (plus clock skew)    */
    MD_SESSION_REQUEST_RUMOR_SKEW,   /* rumor created_at disagrees with issued_at */
} MdSessionRequestFreshness;

const char *md_session_request_freshness_name(MdSessionRequestFreshness f);

/* Host-side check of a parsed request against the host's wall clock and the
 * NIP-17 rumor's own (non-randomized) created_at. Relay history delivery is
 * a separate, transport-level check (see MdNostrDmInfo.live). */
MdSessionRequestFreshness md_session_request_freshness(const MdSessionRequest *req,
                                                       int64_t now_unix,
                                                       int64_t rumor_created_at);

/* Client-side: true only for an acceptance answering exactly `req` — same
 * request_id, a stream token, a non-zero grant within the requested
 * capabilities, and a rumor not older than the request (minus skew). */
bool md_session_accept_matches_request(const MdSessionAccept *acc,
                                       const MdSessionRequest *req,
                                       int64_t rumor_created_at);

/* Helper: convert capability bitfield to/from JSON array of strings. */
uint32_t md_caps_from_strings(const char **strs, int count);
int md_caps_to_strings(uint32_t caps, const char **out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* MD_SESSION_H */
