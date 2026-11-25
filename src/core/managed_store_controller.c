#include "managed_store_controller.h"
#include "nostr.h"
#include "md_time.h"
#include <jansson.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COORDINATE_LIMIT (MD_STORE_MAX_HOSTS + 3)

typedef struct Document {
    struct Document *next;
    MdStoreController *owner;
    MdNostrDocument *subscription;
    char *coordinate;
    bool dropped;
    uint64_t eose[MD_RELAY_TARGET_MAX];
    MdDocumentHistory histories[MD_RELAY_TARGET_MAX];
} Document;

struct MdStoreController {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    MdAccountBinding *client;
    MdManagedStore *store;
    MdNostr *bridge;
    Document *documents;
    size_t count;
    const char *fetch[MD_RELAY_TARGET_MAX], *write[MD_RELAY_TARGET_MAX];
    size_t nfetch, nwrite;
    struct { const char *coordinate, *relay; char id[65]; } pending[MD_RELAY_TARGET_MAX];
    void (*changed)(void *);
    void *data;
};

static void notify(MdStoreController *c) { if (c->changed) c->changed(c->data); }
static void route_changed(void *data) {
    MdStoreController *c = data;
    pthread_mutex_lock(&c->mu);
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    notify(c);
}

static Document *find(MdStoreController *c, const char *coordinate) {
    for (Document *p = c->documents; p; p = p->next)
        if (!strcmp(p->coordinate, coordinate)) return p;
    return NULL;
}

static MdStoreSync sync_locked(MdStoreController *c, Document *p) {
    if (!p || !c->nfetch || !md_account_binding_is_current(c->client)) return MD_SYNC_OFFLINE;
    if (p->dropped) return MD_SYNC_REPLAY_NEEDED;
    if (!p->subscription) return MD_SYNC_OFFLINE;
    MdStoreSync state = MD_SYNC_READY;
    for (size_t i = 0; i < c->nfetch; ++i) {
        uint64_t generation = md_nostr_document_generation(c->bridge, c->fetch[i]);
        if (!generation) { state = MD_SYNC_OFFLINE; continue; }
        if (generation != p->eose[i]) {
            if (state == MD_SYNC_READY) state = MD_SYNC_CATCHUP;
            continue;
        }
        switch (p->histories[i]) {
        case MD_DOCUMENT_EOSE: break;
        case MD_DOCUMENT_AUTH_PENDING: return MD_SYNC_AUTH_PENDING;
        case MD_DOCUMENT_AUTH_REFUSED: return MD_SYNC_AUTH_REFUSED;
        case MD_DOCUMENT_INCOMPLETE: return MD_SYNC_REPLAY_NEEDED;
        case MD_DOCUMENT_REFUSED: return MD_SYNC_REFUSED;
        case MD_DOCUMENT_RECOVERING: if (state == MD_SYNC_READY) state = MD_SYNC_CATCHUP; break;
        }
    }
    return state;
}

static void receive_event(const char *relay, const char *event, void *data) {
    (void)relay;
    Document *p = data;
    MdStoreController *c = p->owner;
    pthread_mutex_lock(&c->mu);
    MdStoreResult r = event ? md_managed_store_receive(c->store, p->coordinate, event, time(NULL)) : MD_STORE_IO;
    /* A relay's reconnect cursor advanced before local admission. Do not
     * let a later EOSE unlock edits when the missing event may be skipped. */
    if (r != MD_STORE_OK && r != MD_STORE_UNCHANGED && r != MD_STORE_CONFLICT && r != MD_STORE_REVOKED) {
        p->dropped = true;
        pthread_cond_broadcast(&c->cv);
    }
    pthread_mutex_unlock(&c->mu);
    if (r != MD_STORE_UNCHANGED) notify(c);
}

static void boundary(const char *relay, uint64_t generation, MdDocumentHistory status, void *data) {
    Document *p = data;
    MdStoreController *c = p->owner;
    pthread_mutex_lock(&c->mu);
    for (size_t i = 0; i < c->nfetch; ++i) {
        if (strcmp(c->fetch[i], relay)) continue;
        p->eose[i] = generation;
        p->histories[i] = status;
        if (status == MD_DOCUMENT_INCOMPLETE) p->dropped = true;
        pthread_cond_broadcast(&c->cv);
        break;
    }
    pthread_mutex_unlock(&c->mu);
    notify(c);
}

static void negative_ok(const char *relay, const char *id, bool ok, const char *reason, void *data) {
    if (ok || !id) return; /* A correlated positive result commits in the waiting publisher. */
    MdStoreController *c = data;
    bool applied = false;
    pthread_mutex_lock(&c->mu);
    for (size_t i = 0; i < c->nwrite; ++i) {
        if (!c->pending[i].coordinate || strcmp(relay, c->pending[i].relay) || strcmp(id, c->pending[i].id)) continue;
        MdStoreRelayResult outcome = reason && !strncmp(reason, "auth-required:", 14) ?
            MD_STORE_RELAY_AUTH_REQUIRED : MD_STORE_RELAY_REJECTED;
        md_managed_store_record_result(c->store, c->pending[i].coordinate, relay, id, outcome);
        applied = true;
        break;
    }
    pthread_mutex_unlock(&c->mu);
    if (applied) notify(c);
}

static bool subscribe(Document *p) {
    MdStoreController *c = p->owner;
    if (!c->bridge) return true;
    pthread_mutex_lock(&c->mu);
    char *filter = md_managed_store_filter(c->store, p->coordinate);
    pthread_mutex_unlock(&c->mu);
    p->subscription = md_nostr_document_open(c->bridge, filter, receive_event, boundary, p);
    free(filter);
    return p->subscription != NULL;
}

bool md_store_controller_watch(MdStoreController *c, const char *coordinate) {
    if (!c || !coordinate) return false;
    Document *p = find(c, coordinate);
    if (p) return p->subscription || !c->bridge || subscribe(p);
    if (c->count >= COORDINATE_LIMIT) return false;
    pthread_mutex_lock(&c->mu);
    char *filter = md_managed_store_filter(c->store, coordinate);
    pthread_mutex_unlock(&c->mu);
    if (!filter) return false;
    free(filter);
    p = calloc(1, sizeof(*p));
    if (!p) return false;
    p->coordinate = strdup(coordinate);
    if (!p->coordinate) { free(p); return false; }
    p->owner = c;
    p->next = c->documents;
    c->documents = p;
    ++c->count;
    return subscribe(p);
}

static void stop(MdStoreController *c) {
    if (!c->bridge) return;
    md_nostr_stop(c->bridge);
    for (Document *p = c->documents; p; p = p->next) {
        md_nostr_document_close(p->subscription);
        p->subscription = NULL;
        memset(p->eose, 0, sizeof(p->eose));
        memset(p->histories, 0, sizeof(p->histories));
        p->dropped = false;
    }
    md_nostr_destroy(c->bridge);
    c->bridge = NULL;
    c->nfetch = c->nwrite = 0;
    memset(c->pending, 0, sizeof(c->pending));
}

bool md_store_controller_replay(MdStoreController *c, const char *coordinate) {
    Document *p = c && coordinate ? find(c, coordinate) : NULL;
    if (!p || !c->bridge) return false;
    md_nostr_document_close(p->subscription);
    p->subscription = NULL;
    memset(p->eose, 0, sizeof(p->eose));
    memset(p->histories, 0, sizeof(p->histories));
    p->dropped = false;
    bool ok = subscribe(p);
    notify(c);
    return ok;
}

MdStoreController *md_store_controller_open(MdAccountBinding *client, const char *directory,
    void (*changed)(void *), void *data) {
    MdManagedStore *store = md_managed_store_open(client, directory);
    if (!store) return NULL;
    MdStoreController *c = calloc(1, sizeof(*c));
    if (!c || pthread_mutex_init(&c->mu, NULL)) { free(c); md_managed_store_close(store); return NULL; }
    if (md_cond_init_monotonic(&c->cv)) {
        pthread_mutex_destroy(&c->mu); free(c); md_managed_store_close(store); return NULL;
    }
    c->client = md_account_binding_ref(client);
    c->store = store;
    c->changed = changed;
    c->data = data;
    if (!md_store_controller_watch(c, MD_STORE_HOSTS) || !md_store_controller_watch(c, MD_STORE_FAVORITES) ||
        !md_store_controller_watch(c, MD_STORE_MANAGEMENT)) { md_store_controller_close(c); return NULL; }
    return c;
}

void md_store_controller_close(MdStoreController *c) {
    if (!c) return;
    stop(c);
    while (c->documents) {
        Document *p = c->documents;
        c->documents = p->next;
        free(p->coordinate);
        free(p);
    }
    md_managed_store_close(c->store);
    md_account_binding_release(c->client);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

bool md_store_controller_start(MdStoreController *c, const MdRelayPlan *bootstrap) {
    if (!c) return false;
    stop(c);
    if (!bootstrap || !bootstrap->count) { notify(c); return true; }
    if (bootstrap->count > MD_RELAY_TARGET_MAX) return false;
    MdIdentity own = {0};
    char *pk = NULL;
    if (md_signer_get_pubkey(md_account_binding_signer(c->client), &pk) != MD_SIGNER_OK || !pk || strlen(pk) != 64) {
        free(pk);
        return false;
    }
    memcpy(own.pubkey, pk, sizeof(own.pubkey));
    free(pk);
    const char *urls[MD_RELAY_TARGET_MAX];
    for (size_t i = 0; i < bootstrap->count; ++i) urls[i] = bootstrap->target[i].url;
    MdNostrConfig cfg = {.owner = c->client, .relay_urls = urls, .relay_count = (int)bootstrap->count,
        .bootstrap_plan = bootstrap, .session_peer = &own, .managed_documents = true};
    MdNostrCallbacks callbacks = {.on_owner_revoked = route_changed, .owner_userdata = c,
                                  .on_document_route = route_changed, .on_document_ok = negative_ok,
                                  .document_userdata = c};
    c->bridge = md_nostr_create(&cfg, &callbacks);
    bool good = c->bridge != NULL;
    if (good) {
        pthread_mutex_lock(&c->mu);
        c->nfetch = md_nostr_document_routes(c->bridge, false, c->fetch);
        c->nwrite = md_nostr_document_routes(c->bridge, true, c->write);
        pthread_mutex_unlock(&c->mu);
        for (Document *p = c->documents; p; p = p->next) if (!subscribe(p)) good = false;
    }
    notify(c);
    return good;
}

MdStoreSync md_store_controller_sync(MdStoreController *c, const char *d) {
    if (!c || !d) return MD_SYNC_OFFLINE;
    pthread_mutex_lock(&c->mu);
    MdStoreSync sync = sync_locked(c, find(c, d));
    pthread_mutex_unlock(&c->mu);
    return sync;
}

MdStoreSync md_store_controller_wait(MdStoreController *c, const char *d, uint64_t deadline) {
    if (!c || !d) return MD_SYNC_OFFLINE;
    pthread_mutex_lock(&c->mu);
    Document *p = find(c, d);
    MdStoreSync state;
    while ((state = sync_locked(c, p)) == MD_SYNC_CATCHUP || state == MD_SYNC_AUTH_PENDING)
        if (md_cond_timedwait_until(&c->cv, &c->mu, deadline) != 0) break;
    pthread_mutex_unlock(&c->mu);
    return state;
}

MdStoreResult md_store_controller_read(MdStoreController *c, const char *d,
    char revision[65], char **winner, char **draft) {
    if (!c) return MD_STORE_INVALID;
    pthread_mutex_lock(&c->mu);
    MdStoreResult r = md_managed_store_read(c->store, d, revision, winner, draft);
    pthread_mutex_unlock(&c->mu);
    return r;
}

static MdStoreResult refresh_barrier(MdStoreController *c, Document *p) {
    if (!p) return MD_STORE_INVALID;
    bool ready = sync_locked(c, p) == MD_SYNC_READY;
    MdStoreResult r = md_managed_store_backfill(c->store, p->coordinate, ready);
    return r != MD_STORE_OK ? r : ready ? MD_STORE_OK : MD_STORE_NOT_READY;
}

MdStoreResult md_store_controller_edit(MdStoreController *c, const char *d,
    MdStoreEdit edit, const char *revision, const char *text) {
    if (!c || !d) return MD_STORE_INVALID;
    pthread_mutex_lock(&c->mu);
    MdStoreResult r = refresh_barrier(c, find(c, d));
    if (r == MD_STORE_OK) {
        int64_t now = time(NULL);
        switch (edit) {
        case MD_EDIT_STAGE: r = md_managed_store_stage(c->store, d, revision, text, c->write, c->nwrite, now); break;
        case MD_EDIT_KEEP_REMOTE: r = md_managed_store_keep_remote(c->store, d, revision, now); break;
        case MD_EDIT_REAPPLY: r = md_managed_store_reapply(c->store, d, revision, text, c->write, c->nwrite, now); break;
        case MD_EDIT_REPLACE_INVALID: r = md_managed_store_replace_invalid(c->store, d, revision, text, c->write, c->nwrite, now); break;
        default: r = MD_STORE_INVALID;
        }
    }
    pthread_mutex_unlock(&c->mu);
    notify(c);
    return r;
}

MdStoreResult md_store_controller_flush(MdStoreController *c, const char *d) {
    if (!c || !d) return MD_STORE_INVALID;
    for (size_t i = 0; i < c->nwrite; ++i) {
        pthread_mutex_lock(&c->mu);
        MdStoreResult r = refresh_barrier(c, find(c, d));
        char *wire = NULL;
        if (r == MD_STORE_OK) r = md_managed_store_outgoing(c->store, d, c->write[i], &wire);
        if (wire) {
            json_t *j = json_loads(wire, 0, NULL);
            const char *id = json_string_value(json_object_get(j, "id"));
            if (id && strlen(id) == 64) {
                memcpy(c->pending[i].id, id, 65);
                c->pending[i].coordinate = find(c, d)->coordinate;
                c->pending[i].relay = c->write[i];
            } else { free(wire); wire = NULL; r = MD_STORE_INVALID; }
            json_decref(j);
        }
        pthread_mutex_unlock(&c->mu);
        if (!wire && r != MD_STORE_OK && r != MD_STORE_UNCHANGED && r != MD_STORE_INVALID &&
            r != MD_STORE_PENDING && r != MD_STORE_PARTIAL) return r;
        if (!wire) continue;
        MdDocumentSendResult result = md_nostr_document_send(c->bridge, c->write[i], wire);
        free(wire);
        if (result == MD_DOCUMENT_ACKNOWLEDGED) {
            pthread_mutex_lock(&c->mu);
            r = md_managed_store_record_result(c->store, d, c->write[i], c->pending[i].id,
                                               MD_STORE_RELAY_ACKNOWLEDGED);
            pthread_mutex_unlock(&c->mu);
            if (r == MD_STORE_IO || r == MD_STORE_REVOKED) { notify(c); return r; }
        }
    }
    char revision[65];
    MdStoreResult r = md_store_controller_read(c, d, revision, NULL, NULL);
    notify(c);
    return r;
}

char *md_store_controller_recents(MdStoreController *c) {
    if (!c) return NULL;
    pthread_mutex_lock(&c->mu);
    char *s = md_managed_store_recents(c->store);
    pthread_mutex_unlock(&c->mu);
    return s;
}

MdStoreResult md_store_controller_record(MdStoreController *c, const char *key, MdRecentOutcome outcome) {
    if (!c) return MD_STORE_INVALID;
    pthread_mutex_lock(&c->mu);
    MdStoreResult r = md_managed_store_record(c->store, key, time(NULL), outcome);
    pthread_mutex_unlock(&c->mu);
    notify(c);
    return r;
}
