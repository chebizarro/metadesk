/*
 * metadesk — session.h
 * Session state machine and lifecycle management.
 * See spec §4 for session negotiation flow.
 */
#ifndef MD_SESSION_H
#define MD_SESSION_H

#include <stdint.h>
#include <stdbool.h>

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
 *   {"type":"session_request","v":1,"capabilities":["video","agent","input"],
 *    "tree_format":"compact","fips_addr":"npub1..."}
 *
 * Accept (host → client DM):
 *   {"type":"session_accept","v":1,"session_id":"<uuid>",
 *    "granted":["video","agent"]}
 */

/* Parsed session request from client */
typedef struct {
    uint32_t    capabilities;      /* bitwise OR of MdCapability */
    MdTreeFormat tree_format;
    char         fips_addr[128];   /* client's npub / FIPS addr  */
} MdSessionRequest;

/* Parsed session accept from host */
typedef struct {
    char         session_id[64];   /* UUID assigned by host      */
    uint32_t     granted;          /* bitwise OR of MdCapability */
} MdSessionAccept;

/* Serialize a session request to a newly-allocated JSON string. Caller frees. */
char *md_session_request_to_json(const MdSessionRequest *req);

/* Parse a session request from JSON. Returns 0 on success, -1 on error. */
int md_session_request_from_json(const char *json, MdSessionRequest *out);

/* Serialize a session accept to a newly-allocated JSON string. Caller frees. */
char *md_session_accept_to_json(const MdSessionAccept *acc);

/* Parse a session accept from JSON. Returns 0 on success, -1 on error. */
int md_session_accept_from_json(const char *json, MdSessionAccept *out);

/* Helper: convert capability bitfield to/from JSON array of strings. */
uint32_t md_caps_from_strings(const char **strs, int count);
int md_caps_to_strings(uint32_t caps, const char **out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* MD_SESSION_H */
