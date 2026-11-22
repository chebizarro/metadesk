#include "host_access.h"
#include <openssl/rand.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static void changed(MdHostAccess *a) {
    ++a->view.revision;
    pthread_cond_broadcast(&a->changed);
    for (size_t i=0; i<8; ++i) if (a->observers[i].notify)
        a->observers[i].notify(a->observers[i].data);
}
static void remove_at(MdHostAccess *a, size_t i) {
    memmove(&a->view.requests[i], &a->view.requests[i+1],
            (--a->view.count - i) * sizeof(a->view.requests[0]));
    memset(&a->view.requests[a->view.count], 0, sizeof(a->view.requests[0]));
    changed(a);
}
static void expire(MdHostAccess *a, int64_t now) {
    for (size_t i = 0; i < a->view.count;) {
        if (now >= a->view.requests[i].request.expires_at) remove_at(a, i);
        else ++i;
    }
}
static int find(MdHostAccess *a, const char *id) {
    for (size_t i = 0; i < a->view.count; ++i)
        if (!strcmp(id, a->view.requests[i].request.request_id)) return (int)i;
    return -1;
}
static bool busy(MdHostAccess *a) {
    if (a->view.occupied) return true;
    for (size_t i = 0; i < a->view.count; ++i)
        if (a->view.requests[i].state != MD_HOST_PENDING) return true;
    return false;
}
static bool fence(MdHostAccess *a, const char *g, uint64_t rev) {
    return !a->stopped && g && !strcmp(g, a->view.generation) && rev == a->view.revision;
}
int md_host_access_init(MdHostAccess *a) {
    memset(a, 0, sizeof(*a));
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return -1;
    for (size_t i = 0; i < sizeof(bytes); ++i)
        snprintf(a->view.generation + i*2, 3, "%02x", bytes[i]);
    a->view.revision = 1;
    if (pthread_mutex_init(&a->mu, NULL)) return -1;
    if (pthread_cond_init(&a->changed, NULL)) { pthread_mutex_destroy(&a->mu); return -1; }
    return 0;
}
void md_host_access_lifecycle(MdHostAccess *a, bool managed, const char *state) {
    pthread_mutex_lock(&a->mu);
    if (a->view.managed != managed || strcmp(a->view.lifecycle, state)) {
        a->view.managed = managed;
        snprintf(a->view.lifecycle, sizeof(a->view.lifecycle), "%s", state);
        changed(a);
    }
    pthread_mutex_unlock(&a->mu);
}
void md_host_access_destroy(MdHostAccess *a) {
    pthread_cond_destroy(&a->changed);
    pthread_mutex_destroy(&a->mu);
}
void md_host_access_stop(MdHostAccess *a) {
    pthread_mutex_lock(&a->mu);
    a->stopped = true;
    a->view.count = 0;
    a->view.revoked = true;
    changed(a);
    pthread_mutex_unlock(&a->mu);
}
static bool request_ok(const char *peer, const MdSessionRequest *r, int64_t now) {
    return peer && strlen(peer) == 64 && r && strlen(r->request_id) == 32 &&
        now < r->expires_at && r->expires_at - now <= MD_SESSION_REQUEST_MAX_LIFETIME_S + MD_SESSION_CLOCK_SKEW_S;
}
int md_host_access_enqueue(MdHostAccess *a, const char *peer, const MdSessionRequest *r, int64_t now) {
    if (!request_ok(peer, r, now)) return -1;
    pthread_mutex_lock(&a->mu);
    expire(a, now);
    int rc = -1;
    if (a->stopped || a->view.occupied || a->view.count == MD_HOST_ACCESS_LIMIT) goto done;
    for (size_t i = 0; i < a->view.count; ++i)
        if (!strcmp(peer, a->view.requests[i].peer) ||
            !strcmp(r->request_id, a->view.requests[i].request.request_id)) goto done;
    MdHostRequest *q = &a->view.requests[a->view.count++];
    *q = (MdHostRequest){ .request = *r };
    memcpy(q->peer, peer, 65);
    changed(a); rc = 0;
done:
    pthread_mutex_unlock(&a->mu); return rc;
}
void md_host_access_snapshot(MdHostAccess *a, int64_t now, MdHostAccessSnapshot *out) {
    pthread_mutex_lock(&a->mu);
    expire(a, now); *out = a->view;
    pthread_mutex_unlock(&a->mu);
}
bool md_host_access_wait(MdHostAccess *a, uint64_t rev) {
    pthread_mutex_lock(&a->mu);
    while (!a->stopped && rev == a->view.revision) {
        int64_t now = (int64_t)time(NULL), next = INT64_MAX;
        expire(a, now);
        if (rev != a->view.revision) break;
        for (size_t i = 0; i < a->view.count; ++i)
            if (a->view.requests[i].request.expires_at < next) next = a->view.requests[i].request.expires_at;
        if (next == INT64_MAX) pthread_cond_wait(&a->changed, &a->mu);
        else { struct timespec deadline = { .tv_sec = (time_t)next };
            pthread_cond_timedwait(&a->changed, &a->mu, &deadline); }
    }
    bool live = !a->stopped;
    pthread_mutex_unlock(&a->mu); return live;
}
int md_host_access_decide(MdHostAccess *a, const char *g, uint64_t rev,
    const char *id, bool approve, bool remember, uint32_t caps, uint32_t supported,
    int64_t now, MdHostRequest *selected) {
    pthread_mutex_lock(&a->mu);
    expire(a, now);
    int i = find(a, id), rc = -1;
    if (!fence(a, g, rev) || i < 0 || a->view.requests[i].state != MD_HOST_PENDING) goto done;
    MdHostRequest *q = &a->view.requests[i];
    if (!approve) { if (selected) *selected = *q; remove_at(a, (size_t)i); rc = 0; goto done; }
    if (busy(a) || !caps || (caps & ~(q->request.capabilities & supported & MD_HOST_ACCESS_CAPS))) goto done;
    q->granted = caps;
    q->human = true;
    q->state = remember ? MD_HOST_REMEMBERING : MD_HOST_APPROVED;
    if (selected) *selected = *q;
    changed(a); rc = 0;
done:
    pthread_mutex_unlock(&a->mu); return rc;
}
int md_host_access_remembered(MdHostAccess *a, const char *id, bool published, int64_t now) {
    pthread_mutex_lock(&a->mu);
    expire(a, now);
    int i = find(a, id), rc = -1;
    if (!a->stopped && i >= 0 && a->view.requests[i].state == MD_HOST_REMEMBERING) {
        a->view.requests[i].state = published ? MD_HOST_APPROVED : MD_HOST_PENDING;
        if (!published) a->view.requests[i].granted = 0;
        changed(a); if (published) rc = 0;
    }
    pthread_mutex_unlock(&a->mu); return rc;
}
int md_host_access_auto(MdHostAccess *a, const char *peer, const MdSessionRequest *r, uint32_t caps, int64_t now) {
    if (!request_ok(peer, r, now) || !caps || (caps & ~(r->capabilities & MD_HOST_ACCESS_CAPS))) return -1;
    pthread_mutex_lock(&a->mu);
    expire(a, now);
    int rc = -1;
    if (!a->stopped && !busy(a)) {
        a->view.occupied = true; a->view.revoked = false;
        a->view.active = (MdHostRequest){ .request = *r, .granted = caps, .state = MD_HOST_APPROVED };
        memcpy(a->view.active.peer, peer, 65);
        changed(a); rc = 0;
    }
    pthread_mutex_unlock(&a->mu); return rc;
}
bool md_host_access_take(MdHostAccess *a, int64_t now, MdHostRequest *out) {
    pthread_mutex_lock(&a->mu);
    expire(a, now);
    if (!a->view.occupied && !a->stopped) for (size_t i = 0; i < a->view.count; ++i)
        if (a->view.requests[i].state == MD_HOST_APPROVED) {
            a->view.active = a->view.requests[i]; a->view.occupied = true; a->view.revoked = false;
            remove_at(a, i); break;
        }
    bool ok = !a->stopped && a->view.occupied && !a->view.revoked &&
        now < a->view.active.request.expires_at;
    if (ok) *out = a->view.active;
    pthread_mutex_unlock(&a->mu); return ok;
}
bool md_host_access_valid(MdHostAccess *a, const char *id) {
    pthread_mutex_lock(&a->mu);
    bool ok = !a->stopped && a->view.occupied && !a->view.revoked &&
        !strcmp(a->view.active.request.request_id, id);
    pthread_mutex_unlock(&a->mu); return ok;
}
void md_host_access_bound(MdHostAccess *a, const char *id, const char *session_id, uint32_t granted) {
    pthread_mutex_lock(&a->mu);
    if (a->view.occupied && !strcmp(a->view.active.request.request_id, id)) {
        a->view.active.granted = granted & a->view.active.request.capabilities & MD_HOST_ACCESS_CAPS;
        snprintf(a->view.session_id, sizeof(a->view.session_id), "%s", session_id); changed(a);
    }
    pthread_mutex_unlock(&a->mu);
}
void md_host_access_release(MdHostAccess *a) {
    pthread_mutex_lock(&a->mu);
    a->view.occupied = false; a->view.revoked = false;
    memset(&a->view.active, 0, sizeof(a->view.active)); a->view.session_id[0] = 0;
    changed(a);
    pthread_mutex_unlock(&a->mu);
}
void md_host_access_cancel(MdHostAccess *a, const char *peer, const char *id) {
    pthread_mutex_lock(&a->mu);
    int i = find(a, id);
    if (i >= 0 && !strcmp(a->view.requests[i].peer, peer)) remove_at(a, (size_t)i);
    if (a->view.occupied && !strcmp(a->view.active.peer, peer) &&
        !strcmp(a->view.active.request.request_id, id) && !a->view.revoked) {
        a->view.revoked = true; changed(a);
    }
    pthread_mutex_unlock(&a->mu);
}
void md_host_access_revoke_peer(MdHostAccess *a, const char *peer) {
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->view.count;)
        if (!strcmp(a->view.requests[i].peer, peer)) remove_at(a, i);
        else ++i;
    if (a->view.occupied && !strcmp(a->view.active.peer, peer) && !a->view.revoked) {
        a->view.revoked = true; changed(a);
    }
    pthread_mutex_unlock(&a->mu);
}
int md_host_access_kick(MdHostAccess *a, const char *g, uint64_t rev, const char *id) {
    pthread_mutex_lock(&a->mu);
    int rc = -1;
    if (fence(a, g, rev) && a->view.occupied && !a->view.revoked &&
        a->view.session_id[0] && !strcmp(id, a->view.session_id)) {
        a->view.revoked = true; changed(a); rc = 0;
    }
    pthread_mutex_unlock(&a->mu); return rc;
}

int md_host_access_watch(MdHostAccess *a, void (*notify)(void *), void *data) {
    pthread_mutex_lock(&a->mu);
    int rc = -1;
    for (size_t i=0; i<8; ++i) if (!a->observers[i].notify) {
        a->observers[i].notify=notify; a->observers[i].data=data; rc=0; break;
    }
    pthread_mutex_unlock(&a->mu); return rc;
}
void md_host_access_unwatch(MdHostAccess *a, void *data) {
    pthread_mutex_lock(&a->mu);
    for (size_t i=0; i<8; ++i) if (a->observers[i].data == data) {
        a->observers[i].notify=NULL; a->observers[i].data=NULL;
    }
    pthread_mutex_unlock(&a->mu);
}
