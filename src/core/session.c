/*
 * metadesk — session.c
 * Session state machine. See spec §4.
 */
#include "session.h"
#include <string.h>

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
    if (!s || s->state != MD_SESSION_REQUESTING)
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

    return (now_ms - s->last_pong_ms) > ((uint64_t)s->keepalive_ms * MD_SESSION_KEEPALIVE_TIMEOUT_MULT);
}
