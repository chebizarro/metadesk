/*
 * metadesk — session.c
 * Session state machine + signaling JSON payloads. See spec §4.
 */
#include "session.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <cjson/cJSON.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

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

    OPENSSL_cleanse(s->stream_token, sizeof(s->stream_token));
    s->stream_deadline_ms = 0;
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

/* ── One-use stream binding ─────────────────────────────────── */

int md_session_prepare_binding(MdSession *s, MdSessionAccept *acc, uint64_t now_ms) {
    if (!s || !acc || s->state != MD_SESSION_NEGOTIATING ||
        !s->session_id[0] || s->stream_token[0] ||
        now_ms > UINT64_MAX - MD_SESSION_TOKEN_LIFETIME_MS)
        return -1;
    uint8_t random[32];
    if (RAND_bytes(random, sizeof(random)) != 1)
        return -1; /* Never authorize with predictable fallback entropy. */
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); i++) {
        s->stream_token[i * 2] = hex[random[i] >> 4];
        s->stream_token[i * 2 + 1] = hex[random[i] & 15];
    }
    OPENSSL_cleanse(random, sizeof(random));
    s->stream_token[64] = '\0';
    s->stream_deadline_ms = now_ms + MD_SESSION_TOKEN_LIFETIME_MS;
    memset(acc, 0, sizeof(*acc));
    memcpy(acc->session_id, s->session_id, sizeof(acc->session_id));
    memcpy(acc->stream_token, s->stream_token, sizeof(acc->stream_token));
    acc->granted = s->capabilities;
    acc->tree_format = s->tree_format;
    return 0;
}

char *md_session_bind_to_json(const MdSessionAccept *acc) {
    if (!acc || !acc->session_id[0] || strlen(acc->stream_token) != 64)
        return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    char *json = NULL;
    if (cJSON_AddStringToObject(root, "type", "session_bind") &&
        cJSON_AddNumberToObject(root, "v", 1) &&
        cJSON_AddStringToObject(root, "session_id", acc->session_id) &&
        cJSON_AddStringToObject(root, "stream_token", acc->stream_token))
        json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

int md_session_consume_binding(MdSession *s, const uint8_t *payload, size_t len,
                               uint64_t now_ms) {
    if (!s || s->state != MD_SESSION_NEGOTIATING || !s->stream_token[0] ||
        !payload || len == 0 || len > MD_SESSION_JSON_MAX ||
        memchr(payload, '\0', len))
        return -1;
    if (now_ms >= s->stream_deadline_ms) {
        OPENSSL_cleanse(s->stream_token, sizeof(s->stream_token));
        return -1;
    }
    char json[MD_SESSION_JSON_MAX + 1];
    memcpy(json, payload, len);
    json[len] = '\0';
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    OPENSSL_cleanse(json, len);
    if (!root) return -1;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    cJSON *sid = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "stream_token");
    /* Exactly four fields rejects duplicate/ambiguous keys and client claims
     * about capabilities. Only the reservation supplies the grant. */
    bool valid = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 4 &&
        cJSON_IsString(type) && strcmp(type->valuestring, "session_bind") == 0 &&
        cJSON_IsNumber(v) && v->valuedouble == 1 &&
        cJSON_IsString(sid) && strcmp(sid->valuestring, s->session_id) == 0 &&
        cJSON_IsString(token) && strlen(token->valuestring) == 64 &&
        CRYPTO_memcmp(token->valuestring, s->stream_token, 64) == 0;
    if (cJSON_IsString(token))
        OPENSSL_cleanse(token->valuestring, strlen(token->valuestring));
    cJSON_Delete(root);
    if (!valid) return -1;
    OPENSSL_cleanse(s->stream_token, sizeof(s->stream_token));
    s->stream_deadline_ms = 0;
    return md_session_activate(s);
}

/* ── Capability helpers ──────────────────────────────────────── */

static const struct { const char *name; MdCapability bit; } cap_table[] = {
    { "video", MD_CAP_VIDEO },
    { "agent", MD_CAP_AGENT },
    { "input", MD_CAP_INPUT },
};
#define CAP_TABLE_LEN (sizeof(cap_table) / sizeof(cap_table[0]))

static bool cap_string_to_bit(const char *name, MdCapability *bit_out) {
    if (!name || !bit_out)
        return false;

    for (size_t j = 0; j < CAP_TABLE_LEN; j++) {
        if (strcmp(name, cap_table[j].name) == 0) {
            *bit_out = cap_table[j].bit;
            return true;
        }
    }
    return false;
}

uint32_t md_caps_from_strings(const char **strs, int count) {
    uint32_t caps = 0;
    for (int i = 0; i < count; i++) {
        MdCapability bit;
        if (cap_string_to_bit(strs[i], &bit))
            caps |= bit;
    }
    return caps;
}

static int parse_capability_array(cJSON *caps, uint32_t *out_caps) {
    if (!out_caps)
        return -1;

    *out_caps = 0;
    if (!caps)
        return 0;
    if (!cJSON_IsArray(caps))
        return -1;

    int count = cJSON_GetArraySize(caps);
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(caps, i);
        if (!cJSON_IsString(item))
            return -1;

        MdCapability bit;
        if (!cap_string_to_bit(item->valuestring, &bit))
            return -1;
        *out_caps |= bit;
    }
    return 0;
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
    if (parse_capability_array(caps, &out->capabilities) < 0) {
        cJSON_Delete(root);
        return -1;
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

    if (acc->stream_token[0] &&
        !cJSON_AddStringToObject(root, "stream_token", acc->stream_token))
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

    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "stream_token");
    if (token) {
        if (!cJSON_IsString(token) || strlen(token->valuestring) != 64) {
            cJSON_Delete(root);
            return -1;
        }
        memcpy(out->stream_token, token->valuestring, 65);
    }

    /* granted */
    cJSON *granted = cJSON_GetObjectItem(root, "granted");
    if (parse_capability_array(granted, &out->granted) < 0) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON_Delete(root);
    return 0;
}
