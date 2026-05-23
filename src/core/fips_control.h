/*
 * metadesk — fips_control.h
 * Thin FIPS daemon control socket client seam.
 *
 * Linux/macOS use a one-shot Unix-domain socket protocol: one
 * line-delimited JSON request per connection, one line-delimited JSON
 * response back from the daemon. This module intentionally targets the
 * FIPS daemon control socket directly and does not use metadesk's legacy
 * fips-nat IPC path.
 */
#ifndef MD_FIPS_CONTROL_H
#define MD_FIPS_CONTROL_H

#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD_FIPS_CONTROL_DEFAULT_TIMEOUT_MS 5000u
#define MD_FIPS_CONTROL_MAX_REQUEST       4096u
#define MD_FIPS_CONTROL_MAX_RESPONSE      (1024u * 1024u)
#define MD_FIPS_CONTROL_PATH_MAX          256u

#define MD_FIPS_CONTROL_RUN_SOCKET        "/run/fips/control.sock"
#define MD_FIPS_CONTROL_XDG_SUFFIX        "/fips/control.sock"
#define MD_FIPS_CONTROL_TMP_SOCKET        "/tmp/fips-control.sock"
#define MD_FIPS_CONTROL_DEFAULT_PEER_POLL_MS 500u


typedef enum {
    MD_FIPS_CONTROL_OK = 0,

    /* The daemon replied with {"status":"error","message":"..."}. */
    MD_FIPS_CONTROL_DAEMON_ERROR,

    /* The daemon could not be reached or did not provide a response. */
    MD_FIPS_CONTROL_DAEMON_UNAVAILABLE,

    /* A response arrived but was not a valid FIPS control response. */
    MD_FIPS_CONTROL_INVALID_RESPONSE,

    /* Caller supplied invalid arguments or a request too large to send. */
    MD_FIPS_CONTROL_INVALID_ARGUMENT,

    /* The local platform has no FIPS control transport implementation here. */
    MD_FIPS_CONTROL_UNSUPPORTED,
} MdFipsControlResult;

typedef struct {
    MdFipsControlResult result;

    /* Resolved socket path used for the request, when available. */
    char *socket_path;

    /* Present on MD_FIPS_CONTROL_OK responses. Caller owns via _free(). */
    cJSON *data;

    /* Present for daemon errors and local failures. Caller owns via _free(). */
    char *message;
} MdFipsControlResponse;

typedef enum {
    MD_FIPS_PEER_READY = 0,
    MD_FIPS_PEER_NOT_FOUND,
    MD_FIPS_PEER_CONVERGING,
    MD_FIPS_PEER_ERROR,
} MdFipsPeerReadinessState;

typedef struct {
    MdFipsPeerReadinessState state;
    MdFipsControlResult control_result;
    char socket_path[MD_FIPS_CONTROL_PATH_MAX];
    char detail[256];
} MdFipsPeerReadiness;

/* Human-readable stable label for diagnostics/logging. */
const char *md_fips_control_result_string(MdFipsControlResult result);
const char *md_fips_peer_readiness_string(MdFipsPeerReadinessState state);

/*
 * Resolve the daemon control socket path for Linux/macOS.
 *
 * If socket_override is non-empty, it wins. Otherwise this follows the FIPS
 * documented client-side default order:
 *   1. /run/fips/control.sock if /run/fips exists
 *   2. $XDG_RUNTIME_DIR/fips/control.sock
 *   3. /tmp/fips-control.sock
 *
 * Returns 0 on success, -1 if arguments are invalid or out is too small.
 */
int md_fips_control_resolve_socket_path(const char *socket_override,
                                        char *out, size_t out_len);

/*
 * Parse one line-delimited FIPS control JSON response.
 *
 * On success, resp->result is either MD_FIPS_CONTROL_OK or
 * MD_FIPS_CONTROL_DAEMON_ERROR. Malformed responses set
 * MD_FIPS_CONTROL_INVALID_RESPONSE with a local diagnostic message.
 * resp must be initialized with md_fips_control_response_init() or {0}
 * before first use. Existing owned fields are released before writing the new
 * result. The caller must release resp with md_fips_control_response_free().
 */
MdFipsControlResult md_fips_control_parse_response(const char *json,
                                                   size_t json_len,
                                                   MdFipsControlResponse *resp);

/*
 * Issue a generic one-shot FIPS control request.
 *
 * command: FIPS control command name, e.g. "show_status".
 * params: optional JSON object; borrowed and not consumed.
 * timeout_ms: 0 uses MD_FIPS_CONTROL_DEFAULT_TIMEOUT_MS.
 *
 * resp must be initialized with md_fips_control_response_init() or {0}
 * before first use. Existing owned fields are released before writing the new
 * result. Returns the same value stored in resp->result.
 */
MdFipsControlResult md_fips_control_request(const char *socket_override,
                                            const char *command,
                                            const cJSON *params,
                                            uint32_t timeout_ms,
                                            MdFipsControlResponse *resp);

/*
 * Poll FIPS daemon control state until a peer is safe to dial over the TUN.
 *
 * This intentionally uses the generic control seam (`show_peers`, then
 * `show_sessions` fallback) rather than parsing FIPS overlay adverts or
 * traversal offers in metadesk. `peer_npub` must be the bech32 npub identity
 * known to the local FIPS daemon.
 */
MdFipsPeerReadinessState
md_fips_control_wait_peer_ready(const char *socket_override,
                                const char *peer_npub,
                                uint32_t total_timeout_ms,
                                uint32_t poll_interval_ms,
                                MdFipsPeerReadiness *out);

/* Initialize a response before first use or before reuse after stack allocation. */
void md_fips_control_response_init(MdFipsControlResponse *resp);

/* Release owned fields; safe on zero-initialized/initialized responses. */
void md_fips_control_response_free(MdFipsControlResponse *resp);

#ifdef __cplusplus
}
#endif

#endif /* MD_FIPS_CONTROL_H */
