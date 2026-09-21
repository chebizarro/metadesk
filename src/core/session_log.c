/*
 * metadesk — session_log.c
 * Signed Nostr session event log (Spec M2.4).
 */
#include "session_log.h"
#include "signer.h"
#include "nostr.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <cjson/cJSON.h>
#include "log.h"
#define MD_LOG_TAG "session_log"

/* ── Event type names ────────────────────────────────────────── */

static const char *event_names[] = {
    [MD_SESSION_LOG_CONNECT]    = "connect",
    [MD_SESSION_LOG_DISCONNECT] = "disconnect",
    [MD_SESSION_LOG_ACTION]     = "action",
    [MD_SESSION_LOG_REQUEST]    = "request",
    [MD_SESSION_LOG_ACCEPT]     = "accept",
    [MD_SESSION_LOG_DENY]       = "deny",
};

#define EVENT_NAME_COUNT (sizeof(event_names) / sizeof(event_names[0]))

const char *md_session_log_event_name(MdSessionLogEventType type) {
    if ((unsigned)type < EVENT_NAME_COUNT && event_names[type])
        return event_names[type];
    return "unknown";
}

/* ── Internal structure ──────────────────────────────────────── */

struct MdSessionLog {
    MdSigner *signer;
    MdNostr  *nostr;
    bool      publish;

    MdSessionLogEntry *entries;
    int capacity;
    int count;     /* total entries ever written */
    int head;      /* next write position in ring */
};

/* ── Lifecycle ───────────────────────────────────────────────── */

MdSessionLog *md_session_log_create(const MdSessionLogConfig *cfg) {
    MdSessionLog *log = calloc(1, sizeof(MdSessionLog));
    if (!log) return NULL;

    log->capacity = (cfg && cfg->capacity > 0)
                    ? cfg->capacity
                    : MD_SESSION_LOG_DEFAULT_CAP;

    log->entries = calloc((size_t)log->capacity, sizeof(MdSessionLogEntry));
    if (!log->entries) {
        free(log);
        return NULL;
    }

    if (cfg) {
        log->signer  = cfg->signer;
        log->nostr   = cfg->nostr;
        log->publish = cfg->publish;
    }

    return log;
}

void md_session_log_destroy(MdSessionLog *log) {
    if (!log) return;

    /* Free signed_json strings */
    int n = log->count < log->capacity ? log->count : log->capacity;
    for (int i = 0; i < n; i++) {
        free(log->entries[i].signed_json);
    }
    free(log->entries);
    free(log);
}

void md_session_log_set_nostr(MdSessionLog *log, MdNostr *nostr) {
    if (log) log->nostr = nostr;
}

/* ── Content builder ─────────────────────────────────────────── */

char *md_session_log_build_content(MdSessionLogEventType type,
                                   const char *session_id,
                                   const char *peer_pubkey,
                                   const char *detail,
                                   int64_t timestamp) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "type", "session_log");
    cJSON_AddStringToObject(root, "event", md_session_log_event_name(type));
    cJSON_AddNumberToObject(root, "ts", (double)timestamp);

    if (session_id && session_id[0])
        cJSON_AddStringToObject(root, "session_id", session_id);
    if (peer_pubkey && peer_pubkey[0])
        cJSON_AddStringToObject(root, "peer", peer_pubkey);
    if (detail && detail[0])
        cJSON_AddStringToObject(root, "detail", detail);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ── Logging ─────────────────────────────────────────────────── */

int md_session_log_event(MdSessionLog *log, MdSessionLogEventType type,
                         const char *session_id, const char *peer_pubkey,
                         const char *detail) {
    if (!log) return -1;

    int64_t ts = (int64_t)time(NULL);

    /* Build content JSON */
    char *content = md_session_log_build_content(type, session_id,
                                                  peer_pubkey, detail, ts);
    if (!content) return -1;

    /* Fill ring buffer entry */
    MdSessionLogEntry *entry = &log->entries[log->head];

    /* Free previous signed_json if overwriting */
    free(entry->signed_json);
    memset(entry, 0, sizeof(*entry));

    entry->type = type;
    entry->timestamp = ts;
    if (session_id)
        strncpy(entry->session_id, session_id, sizeof(entry->session_id) - 1);
    if (peer_pubkey)
        strncpy(entry->peer_pubkey, peer_pubkey, sizeof(entry->peer_pubkey) - 1);
    if (detail)
        strncpy(entry->detail, detail, sizeof(entry->detail) - 1);

    /* Sign as Nostr event if signer is available.  A configured signer
     * makes signing part of the audit guarantee: surface failures to the
     * caller instead of silently recording an unsigned entry. */
    char *signed_json = NULL;
    int sign_status = 0;
    if (log->signer) {
        sign_status = -1;
        char *pk_hex = NULL;
        if (md_signer_get_pubkey(log->signer, &pk_hex) == MD_SIGNER_OK && pk_hex) {
            /* Build the event with the shared Nostr builders; the
             * ["t", topic] tag is the standard filterable topic marker
             * (a "d" tag only has meaning on addressable kinds). */
            NostrEvent *ev = nostr_event_new();
            NostrTags *tags = ev ? nostr_tags_new(0) : NULL;
            if (ev && tags) {
                nostr_event_set_kind(ev, MD_SESSION_LOG_KIND);
                nostr_event_set_content(ev, content);
                nostr_event_set_pubkey(ev, pk_hex);
                nostr_event_set_created_at(ev, ts);
                nostr_tags_append(tags,
                    nostr_tag_new("t", "metadesk-session-log", NULL));
                if (peer_pubkey && peer_pubkey[0])
                    nostr_tags_append(tags,
                        nostr_tag_new("p", peer_pubkey, NULL));
                if (session_id && session_id[0])
                    nostr_tags_append(tags,
                        nostr_tag_new("session", session_id, NULL));
                nostr_event_set_tags(ev, tags); /* takes ownership */

                if (md_nostr_sign_event_json(log->signer, ev,
                                             &signed_json) == 0 &&
                    signed_json)
                    sign_status = 0;
            } else {
                if (tags) nostr_tags_free(tags);
            }
            if (ev) nostr_event_free(ev);
            free(pk_hex);
        }
        if (sign_status != 0) {
            MD_LOG_E("ERROR: failed to sign %s event", md_session_log_event_name(type));
        }
    }

    entry->signed_json = signed_json;

    /* Publish to relays if configured. Publish failures are logged, but the
     * return value reflects signing status so callers can distinguish audit
     * signing failures from best-effort relay delivery. */
    if (log->publish && log->nostr && signed_json) {
        int pret = md_nostr_publish_signed_json(log->nostr, signed_json);
        if (pret != 0) {
            MD_LOG_W("WARNING: failed to publish signed %s event", md_session_log_event_name(type));
        }
    }

    /* Advance ring buffer */
    log->head = (log->head + 1) % log->capacity;
    log->count++;

    free(content);

    MD_LOG_I("%s session=%s peer=%.*s detail=%s", md_session_log_event_name(type), session_id ? session_id : "(none)", peer_pubkey ? 8 : 0, peer_pubkey ? peer_pubkey : "", detail ? detail : "(none)");

    return sign_status;
}

/* ── Query ───────────────────────────────────────────────────── */

int md_session_log_count(const MdSessionLog *log) {
    if (!log) return 0;
    return log->count < log->capacity ? log->count : log->capacity;
}

const MdSessionLogEntry *md_session_log_get(const MdSessionLog *log, int index) {
    if (!log || index < 0) return NULL;

    int available = md_session_log_count(log);
    if (index >= available) return NULL;

    /* Calculate actual position in ring buffer.
     * If buffer hasn't wrapped, entries start at 0.
     * If wrapped, oldest entry is at head (since head is next-write). */
    int start;
    if (log->count <= log->capacity) {
        start = 0;
    } else {
        start = log->head;  /* oldest entry is at head after wrap */
    }

    int pos = (start + index) % log->capacity;
    return &log->entries[pos];
}
