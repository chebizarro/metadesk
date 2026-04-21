/*
 * fips-nat — fipsnat_ipc.h
 * IPC protocol message serialization for fips-nat ↔ metadesk-host.
 *
 * All messages are JSON strings sent over md_ipc_* connections.
 *
 * Commands (host → fips-nat):
 *   {"cmd":"status"}
 *   {"cmd":"discover"}
 *   {"cmd":"punch","peer_ip":"1.2.3.4","peer_port":5678,
 *    "session_id":"0123456789abcdef0123456789abcdef","timeout_ms":10000}
 *   {"cmd":"shutdown"}
 *
 * Responses (fips-nat → host):
 *   {"ok":true, ...}      (payload varies by command)
 *   {"ok":false,"error":"reason"}
 */
#ifndef MD_FIPSNAT_IPC_H
#define MD_FIPSNAT_IPC_H

#include "stun.h"
#include "publish.h"
#include "punch.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Command types ───────────────────────────────────────────── */

typedef enum {
    MD_FIPSNAT_CMD_STATUS   = 0,
    MD_FIPSNAT_CMD_DISCOVER = 1,
    MD_FIPSNAT_CMD_PUNCH    = 2,
    MD_FIPSNAT_CMD_SHUTDOWN = 3,
} MdFipsnatCmdType;

/* Punch request parameters */
typedef struct {
    char     peer_ip[64];
    uint16_t peer_port;
    uint8_t  session_id[16];
    uint32_t timeout_ms;   /* 0 = default */
} MdFipsnatPunchReq;

/* Parsed command */
typedef struct {
    MdFipsnatCmdType  type;
    MdFipsnatPunchReq punch;   /* only valid when type == PUNCH */
} MdFipsnatCmd;

/* ── Command building (host side) ────────────────────────────── */

/* Build JSON command strings. All return malloc'd strings; caller frees. */
char *md_fipsnat_ipc_cmd_status(void);
char *md_fipsnat_ipc_cmd_discover(void);
char *md_fipsnat_ipc_cmd_punch(const char *peer_ip, uint16_t peer_port,
                               const uint8_t session_id[16],
                               uint32_t timeout_ms);
char *md_fipsnat_ipc_cmd_shutdown(void);

/* ── Command parsing (daemon side) ───────────────────────────── */

/* Parse a JSON command string into MdFipsnatCmd.
 * Returns 0 on success, -1 on parse error. */
int md_fipsnat_ipc_parse_command(const char *json, MdFipsnatCmd *out);

/* ── Response building (daemon side) ─────────────────────────── */

/* All return malloc'd JSON strings; caller frees. */
char *md_fipsnat_ipc_error_response(const char *error);
char *md_fipsnat_ipc_ok_response(const char *message);
char *md_fipsnat_ipc_status_response(bool stun_ok, bool published,
                                     const MdNatEndpoint *ep);
char *md_fipsnat_ipc_discover_response(const MdStunResult *stun);
char *md_fipsnat_ipc_punch_response(const MdPunchResult *result);

/* ── Response parsing (host side) ────────────────────────────── */

/* Check if a response indicates success. Returns true if "ok":true. */
bool md_fipsnat_ipc_response_ok(const char *json);

/* Extract error message from a failed response.
 * Returns malloc'd string or NULL. Caller frees. */
char *md_fipsnat_ipc_response_error(const char *json);

/* Parse punch response fields.
 * Returns 0 on success, -1 on error. */
typedef struct {
    int      fd;
    char     peer_ip[64];
    uint16_t peer_port;
    uint16_t local_port;
    uint32_t rtt_ms;
} MdFipsnatPunchResp;

int md_fipsnat_ipc_parse_punch_response(const char *json,
                                        MdFipsnatPunchResp *out);

/* Parse status response fields.
 * Returns 0 on success, -1 on error. */
typedef struct {
    bool     stun_ok;
    bool     published;
    char     ip[64];
    uint16_t port;
    uint16_t fips_port;
    uint16_t punch_port;
} MdFipsnatStatusResp;

int md_fipsnat_ipc_parse_status_response(const char *json,
                                         MdFipsnatStatusResp *out);

#ifdef __cplusplus
}
#endif

#endif /* MD_FIPSNAT_IPC_H */
