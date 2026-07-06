/*
 * metadesk — session.c
 * Session state machine + signaling JSON payloads. See spec §4.
 */
#include "session.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

void md_session_init(MdSession *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->state = MD_SESSION_IDLE;
    s->keepalive_ms = MD_SESSION_KEEPALIVE_DEFAULT_MS;
}

int md_session_request(MdSession *s, const char *peer_npub, uint32_t caps,
                       MdTreeFormat tree_fmt) {
    if (!s || s->state != MD_SESSION_IDLE)
        return -1;

    s->state = MD_SESSION_REQUESTING;
    s->capabilities = caps;
    s->tree_format = tree_fmt;
    if (peer_npub) {
        strncpy(s->peer_npub, peer_npub, sizeof(s->peer_npub) - 1);
        s->peer_npub[sizeof(s->peer_npub) - 1] = '\0';
    }

    return 0;
}

int md_session_accept(MdSession *s, const char *session_id, uint32_t granted_caps) {
    if (!s || (s->state != MD_SESSION_REQUESTING && s->state != MD_SESSION_IDLE))
        return -1;

    s->state = MD_SESSION_NEGOTIATING;
    s->capabilities = granted_caps;
    if (session_id) {
        strncpy(s->session_id, session_id, sizeof(s->session_id) - 1);
        s->session_id[sizeof(s->session_id) - 1] = '\0';
    }

    return 0;
}

int md_session_activate(MdSession *s) {
    if (!s || s->state != MD_SESSION_NEGOTIATING)
        return -1;

    s->state = MD_SESSION_ACTIVE;
    return 0;
}

int md_session_disconnect(MdSession *s) {
    if (!s || s->state == MD_SESSION_IDLE)
        return -1;

    s->state = MD_SESSION_DISCONNECTING;
    return 0;
}

void md_session_reset(MdSession *s) {
    md_session_init(s);
}

bool md_session_on_ping(MdSession *s, uint32_t timestamp_ms) {
    if (!s || s->state != MD_SESSION_ACTIVE)
        return false;

    s->last_ping_ms = timestamp_ms;
    return true; /* caller should send pong */
}

void md_session_on_pong(MdSession *s, uint32_t timestamp_ms) {
    if (!s) return;
    s->last_pong_ms = timestamp_ms;
}

bool md_session_is_timed_out(const MdSession *s, uint64_t now_ms) {
    if (!s || s->state != MD_SESSION_ACTIVE)
        return false;

    if (s->last_pong_ms == 0)
        return false; /* no pongs yet, haven't started keepalive */

    if (now_ms <= s->last_pong_ms)
        return false; /* clock rollback or same timestamp */

    return (now_ms - s->last_pong_ms) > ((uint64_t)s->keepalive_ms * MD_SESSION_KEEPALIVE_TIMEOUT_MULT);
}

/* ── Capability helpers ──────────────────────────────────────── */

static const struct { const char *name; MdCapability bit; } cap_table[] = {
    { "video", MD_CAP_VIDEO },
    { "agent", MD_CAP_AGENT },
    { "input", MD_CAP_INPUT },
};
#define CAP_TABLE_LEN (sizeof(cap_table) / sizeof(cap_table[0]))

uint32_t md_caps_from_strings(const char **strs, int count) {
    uint32_t caps = 0;
    for (int i = 0; i < count; i++) {
        for (size_t j = 0; j < CAP_TABLE_LEN; j++) {
            if (strcmp(strs[i], cap_table[j].name) == 0) {
                caps |= cap_table[j].bit;
                break;
            }
        }
    }
    return caps;
}

int md_caps_to_strings(uint32_t caps, const char **out, int max_out) {
    int n = 0;
    for (size_t j = 0; j < CAP_TABLE_LEN && n < max_out; j++) {
        if (caps & cap_table[j].bit)
            out[n++] = cap_table[j].name;
    }
    return n;
}

/* ── Session request JSON ────────────────────────────────────── */

char *md_session_request_to_json(const MdSessionRequest *req) {
    if (!req) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON *caps_arr = NULL;
    char *json = NULL;

    if (!cJSON_AddStringToObject(root, "type", "session_request") ||
        !cJSON_AddNumberToObject(root, "v", 1))
        goto fail;

    /* capabilities array */
    caps_arr = cJSON_CreateArray();
    if (!caps_arr)
        goto fail;
    const char *cap_strs[8];
    int cap_count = md_caps_to_strings(req->capabilities, cap_strs, 8);
    for (int i = 0; i < cap_count; i++) {
        cJSON *cap = cJSON_CreateString(cap_strs[i]);
        if (!cap || !cJSON_AddItemToArray(caps_arr, cap)) {
            cJSON_Delete(cap);
            goto fail;
        }
    }
    if (!cJSON_AddItemToObject(root, "capabilities", caps_arr))
        goto fail;
    caps_arr = NULL;

    /* tree_format */
    if (!cJSON_AddStringToObject(root, "tree_format",
        req->tree_format == MD_TREE_FORMAT_COMPACT ? "compact" : "json"))
        goto fail;

    /* fips_addr */
    if (req->fips_addr[0] &&
        !cJSON_AddStringToObject(root, "fips_addr", req->fips_addr))
        goto fail;

    json = cJSON_PrintUnformatted(root);

fail:
    cJSON_Delete(caps_arr);
    cJSON_Delete(root);
    return json;
}

int md_session_request_from_json(const char *json, MdSessionRequest *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    /* Verify type */
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "session_request") != 0) {
        cJSON_Delete(root);
        return -1;
    }

    /* capabilities */
    cJSON *caps = cJSON_GetObjectItem(root, "capabilities");
    if (cJSON_IsArray(caps)) {
        int count = cJSON_GetArraySize(caps);
        const char *strs[16];
        int n = 0;
        for (int i = 0; i < count && n < 16; i++) {
            cJSON *item = cJSON_GetArrayItem(caps, i);
            if (cJSON_IsString(item))
                strs[n++] = item->valuestring;
        }
        out->capabilities = md_caps_from_strings(strs, n);
    }

    /* tree_format */
    cJSON *tf = cJSON_GetObjectItem(root, "tree_format");
    if (cJSON_IsString(tf) && strcmp(tf->valuestring, "compact") == 0)
        out->tree_format = MD_TREE_FORMAT_COMPACT;
    else
        out->tree_format = MD_TREE_FORMAT_JSON;

    /* fips_addr */
    cJSON *addr = cJSON_GetObjectItem(root, "fips_addr");
    if (cJSON_IsString(addr)) {
        strncpy(out->fips_addr, addr->valuestring, sizeof(out->fips_addr) - 1);
        out->fips_addr[sizeof(out->fips_addr) - 1] = '\0';
    }

    cJSON_Delete(root);
    return 0;
}

/* ── Session accept JSON ─────────────────────────────────────── */

char *md_session_accept_to_json(const MdSessionAccept *acc) {
    if (!acc) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON *granted_arr = NULL;
    char *json = NULL;

    if (!cJSON_AddStringToObject(root, "type", "session_accept") ||
        !cJSON_AddNumberToObject(root, "v", 1) ||
        !cJSON_AddStringToObject(root, "session_id", acc->session_id))
        goto fail;

    /* granted array */
    granted_arr = cJSON_CreateArray();
    if (!granted_arr)
        goto fail;
    const char *cap_strs[8];
    int cap_count = md_caps_to_strings(acc->granted, cap_strs, 8);
    for (int i = 0; i < cap_count; i++) {
        cJSON *cap = cJSON_CreateString(cap_strs[i]);
        if (!cap || !cJSON_AddItemToArray(granted_arr, cap)) {
            cJSON_Delete(cap);
            goto fail;
        }
    }
    if (!cJSON_AddItemToObject(root, "granted", granted_arr))
        goto fail;
    granted_arr = NULL;

    /* tree_format — confirms which format the host will use */
    if (!cJSON_AddStringToObject(root, "tree_format",
        acc->tree_format == MD_TREE_FORMAT_COMPACT ? "compact" : "json"))
        goto fail;

    json = cJSON_PrintUnformatted(root);

fail:
    cJSON_Delete(granted_arr);
    cJSON_Delete(root);
    return json;
}

int md_session_accept_from_json(const char *json, MdSessionAccept *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    /* Verify type */
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "session_accept") != 0) {
        cJSON_Delete(root);
        return -1;
    }

    /* tree_format (optional — defaults to JSON for backwards compat) */
    cJSON *tf = cJSON_GetObjectItem(root, "tree_format");
    if (cJSON_IsString(tf) && strcmp(tf->valuestring, "compact") == 0)
        out->tree_format = MD_TREE_FORMAT_COMPACT;
    else
        out->tree_format = MD_TREE_FORMAT_JSON;

    /* session_id */
    cJSON *sid = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(sid)) {
        strncpy(out->session_id, sid->valuestring, sizeof(out->session_id) - 1);
        out->session_id[sizeof(out->session_id) - 1] = '\0';
    }

    /* granted */
    cJSON *granted = cJSON_GetObjectItem(root, "granted");
    if (cJSON_IsArray(granted)) {
        int count = cJSON_GetArraySize(granted);
        const char *strs[16];
        int n = 0;
        for (int i = 0; i < count && n < 16; i++) {
            cJSON *item = cJSON_GetArrayItem(granted, i);
            if (cJSON_IsString(item))
                strs[n++] = item->valuestring;
        }
        out->granted = md_caps_from_strings(strs, n);
    }

    cJSON_Delete(root);
    return 0;
}
