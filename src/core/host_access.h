#ifndef MD_HOST_ACCESS_H
#define MD_HOST_ACCESS_H
#include "session.h"
#include <pthread.h>
#define MD_HOST_ACCESS_LIMIT 16
#define MD_HOST_ACCESS_CAPS (MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT | MD_CAP_MONITOR | MD_CAP_CLIPBOARD)
typedef enum { MD_HOST_PENDING, MD_HOST_REMEMBERING, MD_HOST_APPROVED } MdHostRequestState;
typedef struct {
    char peer[65];
    MdSessionRequest request;
    uint32_t granted;
    bool human;
    MdHostRequestState state;
} MdHostRequest;
typedef struct {
    char generation[33];
    uint64_t revision;
    bool managed;
    char lifecycle[24];
    size_t count;
    MdHostRequest requests[MD_HOST_ACCESS_LIMIT];
    bool occupied, revoked;
    MdHostRequest active;
    char session_id[64];
} MdHostAccessSnapshot;
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t changed;
    MdHostAccessSnapshot view;
    bool stopped;
    struct { void (*notify)(void *); void *data; } observers[8];
} MdHostAccess;
/* All times are Unix seconds. The authenticated request window is checked by
 * the caller; local approval has a strict expires_at boundary (no skew grace).
 * Thread safe. No signer, stream, relay or blocking callback work under this
 * lock; observer hooks only signal nonblocking handles. */
int md_host_access_init(MdHostAccess *a);
void md_host_access_lifecycle(MdHostAccess *a, bool managed, const char *state);
void md_host_access_destroy(MdHostAccess *a);
void md_host_access_stop(MdHostAccess *a);
/* Notification hooks ONLY signal nonblocking wake handles. Called under mu;
 * never reenter the state machine or do I/O that can block. Unwatch joins any
 * notification before its handle is closed. */
int md_host_access_watch(MdHostAccess *a, void (*notify)(void *), void *data);
void md_host_access_unwatch(MdHostAccess *a, void *data);
int md_host_access_enqueue(MdHostAccess *a, const char *peer, const MdSessionRequest *r, int64_t now);
void md_host_access_snapshot(MdHostAccess *a, int64_t now, MdHostAccessSnapshot *out);
/* Wait for a revision change or nearest request deadline, not relay polling.
 * false means service shutdown. */
bool md_host_access_wait(MdHostAccess *a, uint64_t revision);
/* Stale generation/revision always conflict. Exact nonzero known caps must be
 * within requested & supported; no implicit input/agent grant. Remember is
 * two phase: claim -> publish outside lock -> finish. Cancel/expiry can win. */
int md_host_access_decide(MdHostAccess *a, const char *generation, uint64_t revision,
    const char *id, bool approve, bool remember, uint32_t caps, uint32_t supported,
    int64_t now, MdHostRequest *selected);
int md_host_access_remembered(MdHostAccess *a, const char *id, bool published, int64_t now);
/* Auto grants still reserve the same single active slot. */
int md_host_access_auto(MdHostAccess *a, const char *peer, const MdSessionRequest *r,
                         uint32_t caps, int64_t now);
bool md_host_access_take(MdHostAccess *a, int64_t now, MdHostRequest *out);
bool md_host_access_valid(MdHostAccess *a, const char *request_id);
void md_host_access_bound(MdHostAccess *a, const char *request_id, const char *session_id, uint32_t granted);
void md_host_access_release(MdHostAccess *a);
void md_host_access_cancel(MdHostAccess *a, const char *peer, const char *id);
void md_host_access_revoke_peer(MdHostAccess *a, const char *peer);
int md_host_access_kick(MdHostAccess *a, const char *generation, uint64_t revision, const char *session_id);
#endif
