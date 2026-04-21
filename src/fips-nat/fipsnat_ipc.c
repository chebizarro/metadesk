/*
 * fips-nat — fipsnat_ipc.c
 * IPC protocol message serialization.
 *
 * Uses cJSON for JSON building/parsing. All functions are
 * self-contained and testable without network access.
 */
#include "fipsnat_ipc.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────── */

/* Convert 16-byte session ID to 32-char hex string */
static void session_id_to_hex(const uint8_t id[16], char hex[33]) {
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", id[i]);
    hex[32] = '\0';
}

/* Convert 32-char hex string to 16-byte session ID. Returns 0 on success. */
static int hex_to_session_id(const char *hex, uint8_t id[16]) {
    if (!hex || strlen(hex) != 32)
        return -1;
    for (int i = 0; i < 16; i++) {
        unsigned int b;
        if (sscanf(hex + i * 2, "%2x", &b) != 1)
            return -1;
        id[i] = (uint8_t)b;
    }
    return 0;
}

/* Helper: render cJSON to malloc'd string and delete the object */
static char *json_print_and_delete(cJSON *obj) {
    if (!obj) return NULL;
    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return str;
}

/* ── Command building (host side) ────────────────────────────── */

char *md_fipsnat_ipc_cmd_status(void) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "cmd", "status");
    return json_print_and_delete(obj);
}

char *md_fipsnat_ipc_cmd_discover(void) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "cmd", "discover");
    return json_print_and_delete(obj);
}

char *md_fipsnat_ipc_cmd_punch(const char *peer_ip, uint16_t peer_port,
                               const uint8_t session_id[16],
                               uint32_t timeout_ms) {
    if (!peer_ip || !session_id) return NULL;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "cmd", "punch");
    cJSON_AddStringToObject(obj, "peer_ip", peer_ip);
    cJSON_AddNumberToObject(obj, "peer_port", peer_port);

    char hex[33];
    session_id_to_hex(session_id, hex);
    cJSON_AddStringToObject(obj, "session_id", hex);

    if (timeout_ms > 0)
        cJSON_AddNumberToObject(obj, "timeout_ms", timeout_ms);

    return json_print_and_delete(obj);
}

char *md_fipsnat_ipc_cmd_shutdown(void) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "cmd", "shutdown");
    return json_print_and_delete(obj);
}

/* ── Command parsing (daemon side) ───────────────────────────── */

int md_fipsnat_ipc_parse_command(const char *json, MdFipsnatCmd *out) {
    if (!json || !out) return -1;

    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd) || !cmd->valuestring) {
        cJSON_Delete(root);
        return -1;
    }

    const char *cmd_str = cmd->valuestring;

    if (strcmp(cmd_str, "status") == 0) {
        out->type = MD_FIPSNAT_CMD_STATUS;
    }
    else if (strcmp(cmd_str, "discover") == 0) {
        out->type = MD_FIPSNAT_CMD_DISCOVER;
    }
    else if (strcmp(cmd_str, "shutdown") == 0) {
        out->type = MD_FIPSNAT_CMD_SHUTDOWN;
    }
    else if (strcmp(cmd_str, "punch") == 0) {
        out->type = MD_FIPSNAT_CMD_PUNCH;

        /* Parse punch parameters */
        cJSON *ip = cJSON_GetObjectItemCaseSensitive(root, "peer_ip");
        cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "peer_port");
        cJSON *sid = cJSON_GetObjectItemCaseSensitive(root, "session_id");

        if (!cJSON_IsString(ip) || !cJSON_IsNumber(port) || !cJSON_IsString(sid)) {
            cJSON_Delete(root);
            return -1;
        }

        strncpy(out->punch.peer_ip, ip->valuestring,
                sizeof(out->punch.peer_ip) - 1);
        out->punch.peer_port = (uint16_t)port->valuedouble;

        if (hex_to_session_id(sid->valuestring, out->punch.session_id) < 0) {
            cJSON_Delete(root);
            return -1;
        }

        cJSON *timeout = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");
        if (cJSON_IsNumber(timeout))
            out->punch.timeout_ms = (uint32_t)timeout->valuedouble;
    }
    else {
        cJSON_Delete(root);
        return -1;
    }

    cJSON_Delete(root);
    return 0;
}

/* ── Response building (daemon side) ─────────────────────────── */

char *md_fipsnat_ipc_error_response(const char *error) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddBoolToObject(obj, "ok", 0);
    cJSON_AddStringToObject(obj, "error", error ? error : "unknown error");
    return json_print_and_delete(obj);
}

char *md_fipsnat_ipc_ok_response(const char *message) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddBoolToObject(obj, "ok", 1);
    if (message)
        cJSON_AddStringToObject(obj, "message", message);
    return json_print_and_delete(obj);
}

char *md_fipsnat_ipc_status_response(bool stun_ok, bool published,
                                     const MdNatEndpoint *ep) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddBoolToObject(obj, "ok", 1);
    cJSON_AddBoolToObject(obj, "stun_ok", stun_ok ? 1 : 0);
    cJSON_AddBoolToObject(obj, "published", published ? 1 : 0);

    if (ep && stun_ok) {
        cJSON_AddStringToObject(obj, "ip", ep->stun.ip);
        cJSON_AddNumberToObject(obj, "port", ep->stun.port);
        cJSON_AddNumberToObject(obj, "fips_port", ep->fips_port);
        cJSON_AddNumberToObject(obj, "punch_port", ep->punch_port);
    }

    return json_print_and_delete(obj);
}

char *md_fipsnat_ipc_discover_response(const MdStunResult *stun) {
    if (!stun) return md_fipsnat_ipc_error_response("null result");

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddBoolToObject(obj, "ok", 1);
    cJSON_AddStringToObject(obj, "ip", stun->ip);
    cJSON_AddNumberToObject(obj, "port", stun->port);
    cJSON_AddBoolToObject(obj, "ipv6", stun->is_ipv6 ? 1 : 0);

    return json_print_and_delete(obj);
}

char *md_fipsnat_ipc_punch_response(const MdPunchResult *result) {
    if (!result) return md_fipsnat_ipc_error_response("null result");

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddBoolToObject(obj, "ok", 1);
    cJSON_AddNumberToObject(obj, "fd", result->fd);
    cJSON_AddStringToObject(obj, "peer_ip", result->peer_ip);
    cJSON_AddNumberToObject(obj, "peer_port", result->peer_port);
    cJSON_AddNumberToObject(obj, "local_port", result->local_port);
    cJSON_AddNumberToObject(obj, "rtt_ms", result->rtt_ms);

    return json_print_and_delete(obj);
}

/* ── Response parsing (host side) ────────────────────────────── */

bool md_fipsnat_ipc_response_ok(const char *json) {
    if (!json) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    bool result = cJSON_IsTrue(ok);
    cJSON_Delete(root);
    return result;
}

char *md_fipsnat_ipc_response_error(const char *json) {
    if (!json) return NULL;
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;

    char *result = NULL;
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(err) && err->valuestring)
        result = strdup(err->valuestring);

    cJSON_Delete(root);
    return result;
}

int md_fipsnat_ipc_parse_punch_response(const char *json,
                                        MdFipsnatPunchResp *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *fd = cJSON_GetObjectItemCaseSensitive(root, "fd");
    cJSON *ip = cJSON_GetObjectItemCaseSensitive(root, "peer_ip");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "peer_port");
    cJSON *lport = cJSON_GetObjectItemCaseSensitive(root, "local_port");
    cJSON *rtt = cJSON_GetObjectItemCaseSensitive(root, "rtt_ms");

    if (cJSON_IsNumber(fd)) out->fd = (int)fd->valuedouble;
    if (cJSON_IsString(ip) && ip->valuestring)
        strncpy(out->peer_ip, ip->valuestring, sizeof(out->peer_ip) - 1);
    if (cJSON_IsNumber(port)) out->peer_port = (uint16_t)port->valuedouble;
    if (cJSON_IsNumber(lport)) out->local_port = (uint16_t)lport->valuedouble;
    if (cJSON_IsNumber(rtt)) out->rtt_ms = (uint32_t)rtt->valuedouble;

    cJSON_Delete(root);
    return 0;
}

int md_fipsnat_ipc_parse_status_response(const char *json,
                                         MdFipsnatStatusResp *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *stun_ok = cJSON_GetObjectItemCaseSensitive(root, "stun_ok");
    cJSON *published = cJSON_GetObjectItemCaseSensitive(root, "published");
    cJSON *ip = cJSON_GetObjectItemCaseSensitive(root, "ip");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
    cJSON *fport = cJSON_GetObjectItemCaseSensitive(root, "fips_port");
    cJSON *pport = cJSON_GetObjectItemCaseSensitive(root, "punch_port");

    out->stun_ok = cJSON_IsTrue(stun_ok);
    out->published = cJSON_IsTrue(published);
    if (cJSON_IsString(ip) && ip->valuestring)
        strncpy(out->ip, ip->valuestring, sizeof(out->ip) - 1);
    if (cJSON_IsNumber(port)) out->port = (uint16_t)port->valuedouble;
    if (cJSON_IsNumber(fport)) out->fips_port = (uint16_t)fport->valuedouble;
    if (cJSON_IsNumber(pport)) out->punch_port = (uint16_t)pport->valuedouble;

    cJSON_Delete(root);
    return 0;
}
