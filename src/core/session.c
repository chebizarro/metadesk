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
    { "monitor", MD_CAP_MONITOR },
    { "clipboard", MD_CAP_CLIPBOARD },
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

/* ── Request correlation fields (md-6ns) ─────────────────────── */

static bool is_request_id(const char *s) {
    if (!s || strlen(s) != MD_SESSION_REQUEST_ID_LEN) return false;
    for (const char *p = s; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

/* Absent: 0. Present and valid: 1. Present and invalid: -1. */
static int parse_request_id(cJSON *root, char out[MD_SESSION_REQUEST_ID_LEN + 1]) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    if (!id) return 0;
    if (!cJSON_IsString(id) || !is_request_id(id->valuestring)) return -1;
    memcpy(out, id->valuestring, MD_SESSION_REQUEST_ID_LEN + 1);
    return 1;
}

/* Integral unix seconds within the exactly representable JSON range. */
static int parse_unix_time(cJSON *root, const char *key, int64_t *out) {
    cJSON *t = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!t) return 0;
    if (!cJSON_IsNumber(t) || t->valuedouble < 1 ||
        t->valuedouble > 9007199254740991.0 ||
        t->valuedouble != (double)(int64_t)t->valuedouble)
        return -1;
    *out = (int64_t)t->valuedouble;
    return 1;
}

int md_session_request_prepare(MdSessionRequest *req, int64_t now_unix,
                               uint32_t lifetime_s) {
    if (!req || now_unix <= 0) return -1;
    req->request_id[0] = '\0';
    req->issued_at = req->expires_at = 0;
    if (lifetime_s < 1) lifetime_s = 1;
    if (lifetime_s > MD_SESSION_REQUEST_MAX_LIFETIME_S)
        lifetime_s = MD_SESSION_REQUEST_MAX_LIFETIME_S;
    uint8_t random[MD_SESSION_REQUEST_ID_LEN / 2];
    if (RAND_bytes(random, sizeof(random)) != 1)
        return -1; /* No predictable fallback: a guessable id is not a nonce. */
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); i++) {
        req->request_id[i * 2] = hex[random[i] >> 4];
        req->request_id[i * 2 + 1] = hex[random[i] & 15];
    }
    req->request_id[MD_SESSION_REQUEST_ID_LEN] = '\0';
    req->stream_tls = true;
    req->issued_at = now_unix;
    req->expires_at = now_unix + (int64_t)lifetime_s;
    return 0;
}

const char *md_session_request_freshness_name(MdSessionRequestFreshness f) {
    switch (f) {
    case MD_SESSION_REQUEST_FRESH: return "fresh";
    case MD_SESSION_REQUEST_UNCORRELATED: return "uncorrelated";
    case MD_SESSION_REQUEST_BAD_WINDOW: return "bad_window";
    case MD_SESSION_REQUEST_FUTURE: return "future";
    case MD_SESSION_REQUEST_EXPIRED: return "expired";
    case MD_SESSION_REQUEST_RUMOR_SKEW: return "rumor_skew";
    }
    return "unknown";
}

MdSessionRequestFreshness md_session_request_freshness(const MdSessionRequest *req,
                                                       int64_t now_unix,
                                                       int64_t rumor_created_at) {
    if (!req || !is_request_id(req->request_id) ||
        req->issued_at <= 0 || req->expires_at <= 0)
        return MD_SESSION_REQUEST_UNCORRELATED;
    if (req->expires_at <= req->issued_at ||
        req->expires_at - req->issued_at > MD_SESSION_REQUEST_MAX_LIFETIME_S)
        return MD_SESSION_REQUEST_BAD_WINDOW;
    if (req->issued_at > now_unix + MD_SESSION_CLOCK_SKEW_S)
        return MD_SESSION_REQUEST_FUTURE;
    if (now_unix > req->expires_at + MD_SESSION_CLOCK_SKEW_S)
        return MD_SESSION_REQUEST_EXPIRED;
    if (rumor_created_at < req->issued_at - MD_SESSION_CLOCK_SKEW_S ||
        rumor_created_at > req->issued_at + MD_SESSION_CLOCK_SKEW_S)
        return MD_SESSION_REQUEST_RUMOR_SKEW;
    return MD_SESSION_REQUEST_FRESH;
}

bool md_session_accept_matches_request(const MdSessionAccept *acc,
                                       const MdSessionRequest *req,
                                       int64_t rumor_created_at) {
    return acc && req && is_request_id(req->request_id) &&
        strcmp(acc->request_id, req->request_id) == 0 &&
        acc->session_id[0] && strlen(acc->stream_token) == 64 &&
        acc->granted != 0 && (acc->granted & ~req->capabilities) == 0 &&
        (!acc->quality_present || md_quality_accepts(&req->quality,&acc->quality)) &&
        (acc->clipboard_directions & ~req->clipboard_directions)==0 &&
        ((acc->granted & MD_CAP_CLIPBOARD) ? acc->clipboard_directions!=0 : acc->clipboard_directions==0) &&
        req->issued_at > 0 &&
        rumor_created_at >= req->issued_at - MD_SESSION_CLOCK_SKEW_S;
}

bool md_session_json_strings_safe(const char *json) {
    if (!json) return false;
    for (const char *p=json;*p;p++) {
        if (*p!='\\') continue;
        if (!*++p) break;
        if (*p=='u' && !strncmp(p+1,"0000",4)) return false;
    }
    return true;
}
static int clipboard_read(const cJSON *root,uint8_t *out) {
    *out=0; const cJSON *p=NULL,*item;
    cJSON_ArrayForEach(item,root) if (item->string && !strcmp(item->string,"clipboard")) {
        if (p) return -1; p=item;
    }
    if (!p) return 0;
    int d=cJSON_IsString(p) ? md_clipboard_directions_parse(p->valuestring) : -1;
    if (d<0) return -1; *out=(uint8_t)d; return 0;
}
/* ── Session request JSON ────────────────────────────────────── */

char *md_session_request_to_json(const MdSessionRequest *req) {
    if (!req) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON *caps_arr = NULL;
    char *json = NULL;
    if (req->stream_tls && !cJSON_AddStringToObject(root, "stream_tls", "tls13-sha256"))
        goto fail;

    if (!cJSON_AddStringToObject(root, "type", "session_request") ||
        !cJSON_AddNumberToObject(root, "v", 1))
        goto fail;
    if (req->request_id[0] &&
        (!is_request_id(req->request_id) ||
         !cJSON_AddStringToObject(root, "request_id", req->request_id) ||
         !cJSON_AddNumberToObject(root, "issued_at", (double)req->issued_at) ||
         !cJSON_AddNumberToObject(root, "expires_at", (double)req->expires_at)))
        goto fail;

    if (req->clipboard_directions && (req->clipboard_directions>3 ||
        !cJSON_AddStringToObject(root,"clipboard",md_clipboard_directions_name(req->clipboard_directions)))) goto fail;
    if (req->output[0] && (!md_capture_output_id_valid(req->output) ||
        !cJSON_AddStringToObject(root,"output",req->output))) goto fail;
    if (req->quality_present && md_quality_write(root, &req->quality)) goto fail;

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
    if (!out || !md_session_json_strings_safe(json)) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithOpts(json,NULL,true);
    if (!root) return -1;

    /* Verify type */
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "session_request") != 0) {
        cJSON_Delete(root);
        return -1;
    }

    if (clipboard_read(root,&out->clipboard_directions)) { cJSON_Delete(root); return -1; }
    if (md_quality_read(root, &out->quality, &out->quality_present)) {
        cJSON_Delete(root); return -1;
    }

    cJSON *output=cJSON_GetObjectItemCaseSensitive(root,"output");
    if (output) {
        if (!cJSON_IsString(output) || strlen(output->valuestring)>=sizeof(out->output) ||
            !md_capture_output_id_valid(output->valuestring) || strstr(json,"\\u0000")) {
            cJSON_Delete(root); return -1;
        }
        strcpy(out->output,output->valuestring);
    }
    /* Correlation fields are optional on the wire (so a legacy request
     * parses and can be reported) but strictly typed when present. */
    if (parse_request_id(root, out->request_id) < 0 ||
        parse_unix_time(root, "issued_at", &out->issued_at) < 0 ||
        parse_unix_time(root, "expires_at", &out->expires_at) < 0) {
        cJSON_Delete(root);
        memset(out, 0, sizeof(*out));
        return -1;
    }

    cJSON *tls = cJSON_GetObjectItemCaseSensitive(root, "stream_tls");
    if (tls && (!cJSON_IsString(tls) || strcmp(tls->valuestring, "tls13-sha256"))) {
        cJSON_Delete(root);
        return -1;
    }
    out->stream_tls = tls != NULL;

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

    if (acc->request_id[0] &&
        (!is_request_id(acc->request_id) ||
         !cJSON_AddStringToObject(root, "request_id", acc->request_id)))
        goto fail;

    if (acc->stream_token[0] &&
        !cJSON_AddStringToObject(root, "stream_token", acc->stream_token))
        goto fail;

    if (acc->tls_cert_sha256[0] &&
        !cJSON_AddStringToObject(root, "tls_cert_sha256", acc->tls_cert_sha256))
        goto fail;

    if (acc->clipboard_directions && (acc->clipboard_directions>3 ||
        !cJSON_AddStringToObject(root,"clipboard",md_clipboard_directions_name(acc->clipboard_directions)))) goto fail;
    if (acc->outputs_present && md_session_outputs_write(root,&acc->outputs)) goto fail;
    if (acc->quality_present && md_quality_write(root, &acc->quality)) goto fail;

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
    if (!out || !md_session_json_strings_safe(json)) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithOpts(json,NULL,true);
    if (!root) return -1;

    /* Verify type */
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "session_accept") != 0) {
        cJSON_Delete(root);
        return -1;
    }

    if (md_quality_read(root, &out->quality, &out->quality_present)) {
        cJSON_Delete(root); return -1;
    }

    if (md_session_outputs_read(root,&out->outputs,&out->outputs_present)) { cJSON_Delete(root); return -1; }
    if (clipboard_read(root,&out->clipboard_directions)) { cJSON_Delete(root); return -1; }
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

    if (parse_request_id(root, out->request_id) < 0) {
        cJSON_Delete(root);
        memset(out, 0, sizeof(*out));
        return -1;
    }

    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "stream_token");
    if (token) {
        if (!cJSON_IsString(token) || strlen(token->valuestring) != 64) {
            cJSON_Delete(root);
            return -1;
        }
        memcpy(out->stream_token, token->valuestring, 65);
    }

    cJSON *pin = cJSON_GetObjectItemCaseSensitive(root, "tls_cert_sha256");
    if (pin) {
        if (!cJSON_IsString(pin) || strlen(pin->valuestring) != 64 ||
            strspn(pin->valuestring, "0123456789abcdef") != 64) {
            cJSON_Delete(root);
            return -1;
        }
        memcpy(out->tls_cert_sha256, pin->valuestring, 65);
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

/* ── Correlated admission refusal ────────────────────────────── */

const char *md_session_reject_message(const MdSessionReject *reject) {
    if (!reject) return NULL;
    if (!strcmp(reject->reason,"quality_unsupported"))
        return "host capture/encoder cannot honor requested quality; choose a larger resolution cap or lower frame rate";
    if (!strcmp(reject->reason,"output_unavailable"))
        return "requested output unavailable or monitor grant missing; Linux portal switching requires new local consent";
    if (!strcmp(reject->reason, "tls_required"))
        return "Pinned TLS 1.3 required; upgrade both host and client";
    if (!strcmp(reject->reason, "capture_unavailable"))
        return "host screen capture unavailable; authorize screen sharing on the host and retry";
    if (!strcmp(reject->reason, "request_refused"))
        return "host refused session; check current access grants and retry with a fresh request";
    return NULL;
}

char *md_session_reject_to_json(const MdSessionReject *reject) {
    if (!reject || !is_request_id(reject->request_id) ||
        !md_session_reject_message(reject)) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    char *json = NULL;
    if (cJSON_AddStringToObject(root, "type", "session_reject") &&
        cJSON_AddNumberToObject(root, "v", 1) &&
        cJSON_AddStringToObject(root, "request_id", reject->request_id) &&
        cJSON_AddStringToObject(root, "reason", reject->reason))
        json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

int md_session_reject_from_json(const char *json, MdSessionReject *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (!root) return -1;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    bool valid = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 4 &&
        cJSON_IsString(type) && !strcmp(type->valuestring, "session_reject") &&
        cJSON_IsNumber(v) && v->valuedouble == 1 &&
        parse_request_id(root, out->request_id) == 1 &&
        cJSON_IsString(reason) && strlen(reason->valuestring) < sizeof(out->reason);
    if (valid) {
        strcpy(out->reason, reason->valuestring);
        valid = md_session_reject_message(out) != NULL;
    }
    cJSON_Delete(root);
    if (!valid) memset(out, 0, sizeof(*out));
    return valid ? 0 : -1;
}

bool md_session_reject_matches_request(const MdSessionReject *reject,
                                       const MdSessionRequest *req,
                                       int64_t rumor_created_at) {
    return reject && req && is_request_id(req->request_id) &&
        !strcmp(reject->request_id, req->request_id) &&
        md_session_reject_message(reject) && req->issued_at > 0 &&
        rumor_created_at >= req->issued_at - MD_SESSION_CLOCK_SKEW_S;
}

int md_session_cancel_from_json(const char *json, char request_id[MD_SESSION_REQUEST_ID_LEN + 1]) {
    if (!json || !request_id || strlen(json) > MD_SESSION_JSON_MAX) return -1;
    request_id[0] = 0;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (!root) return -1;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    bool ok = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 3 &&
        cJSON_IsString(type) && !strcmp(type->valuestring, "session_cancel") &&
        cJSON_IsNumber(v) && v->valuedouble == 1 &&
        cJSON_IsString(id) && is_request_id(id->valuestring);
    if (ok) memcpy(request_id, id->valuestring, MD_SESSION_REQUEST_ID_LEN + 1);
    cJSON_Delete(root);
    return ok ? 0 : -1;
}

int md_session_outputs_write(cJSON *root,const MdCaptureOutputs *outputs) {
    if (!root || !outputs || outputs->count>MD_CAPTURE_OUTPUTS_MAX) return -1;
    cJSON *state=cJSON_AddObjectToObject(root,"displays");
    if (!state || !cJSON_AddBoolToObject(state,"switch_supported",outputs->switch_supported) ||
        !cJSON_AddStringToObject(state,"selected",outputs->selected)) return -1;
    cJSON *array=cJSON_AddArrayToObject(state,"outputs"); if (!array) return -1;
    for (size_t i=0;i<outputs->count;i++) {
        const MdCaptureOutput *o=&outputs->items[i]; cJSON *item=cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(array,item)) { cJSON_Delete(item); return -1; }
        if (!cJSON_AddStringToObject(item,"id",o->id) || !cJSON_AddStringToObject(item,"name",o->name) ||
            !cJSON_AddNumberToObject(item,"x",o->rect.x) || !cJSON_AddNumberToObject(item,"y",o->rect.y) ||
            !cJSON_AddNumberToObject(item,"width",o->rect.width) || !cJSON_AddNumberToObject(item,"height",o->rect.height)) return -1;
    }
    return 0;
}
int md_session_outputs_read(const cJSON *root,MdCaptureOutputs *out,bool *present) {
    memset(out,0,sizeof(*out)); *present=false;
    cJSON *state=cJSON_GetObjectItemCaseSensitive(root,"displays"); if (!state) return 0;
    cJSON *array=cJSON_GetObjectItemCaseSensitive(state,"outputs");
    cJSON *selected=cJSON_GetObjectItemCaseSensitive(state,"selected");
    cJSON *supported=cJSON_GetObjectItemCaseSensitive(state,"switch_supported");
    if (!cJSON_IsObject(state) || !cJSON_IsArray(array) || cJSON_GetArraySize(array)>MD_CAPTURE_OUTPUTS_MAX ||
        !cJSON_IsString(selected) || strlen(selected->valuestring)>=sizeof(out->selected) ||
        (selected->valuestring[0] && !md_capture_output_id_valid(selected->valuestring)) || !cJSON_IsBool(supported)) return -1;
    out->switch_supported=cJSON_IsTrue(supported); strcpy(out->selected,selected->valuestring);
    cJSON *item;
    cJSON_ArrayForEach(item,array) {
        MdCaptureOutput *o=&out->items[out->count];
        cJSON *id=cJSON_GetObjectItemCaseSensitive(item,"id"), *name=cJSON_GetObjectItemCaseSensitive(item,"name");
        if (!cJSON_IsString(id) || strlen(id->valuestring)>=sizeof(o->id) || !md_capture_output_id_valid(id->valuestring) ||
            !cJSON_IsString(name) || strlen(name->valuestring)>=sizeof(o->name)) return -1;
        for (size_t i=0;i<out->count;i++) if (!strcmp(out->items[i].id,id->valuestring)) return -1;
        strcpy(o->id,id->valuestring); strcpy(o->name,name->valuestring);
        const char *keys[]={"x","y","width","height"};
        double vals[4];
        for (size_t k=0;k<4;k++) {
            cJSON *n=cJSON_GetObjectItemCaseSensitive(item,keys[k]);
            if (!cJSON_IsNumber(n) || n->valuedouble<(k<2 ? INT32_MIN : 0) ||
                n->valuedouble>INT32_MAX || n->valuedouble!=(double)(int32_t)n->valuedouble) return -1;
            vals[k]=n->valuedouble;
        }
        o->rect=(MdCaptureRect){(int32_t)vals[0],(int32_t)vals[1],(uint32_t)vals[2],(uint32_t)vals[3]}; out->count++;
    }
    *present=true; return 0;
}
char *md_session_outputs_json(const MdCaptureOutputs *outputs,const char *error) {
    cJSON *root=cJSON_CreateObject(); char *s=NULL;
    if (root && cJSON_AddStringToObject(root,"type","outputs") && !md_session_outputs_write(root,outputs) &&
        (!error || cJSON_AddStringToObject(root,"error",error))) s=cJSON_PrintUnformatted(root);
    cJSON_Delete(root); return s;
}
