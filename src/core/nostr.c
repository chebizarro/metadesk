/*
 * metadesk — nostr.c
 * Thin bridge to nostrc library for Nostr protocol operations.
 *
 * PROTOCOL MODEL (NIP-01):
 *   Nostr is event-driven pub/sub over persistent WebSocket connections.
 *   Relay connections last the app's lifetime. This bridge:
 *   1. Creates a NostrSimplePool and connects to all configured relays
 *   2. Registers live subscriptions (REQ) that persist until teardown
 *   3. Routes incoming events to metadesk callbacks asynchronously
 *   4. Publishes events (EVENT) for session signaling and transport info
 *
 * SIGNER INTEGRATION:
 *   All signing and encryption is delegated to MdSigner. The signer
 *   may be direct-key (in-process secp256k1), NIP-46 (remote bunker),
 *   NIP-55L (D-Bus daemon), or NIP-5F (Unix socket daemon).
 *
 *   For backward compatibility, sk_hex in MdNostrConfig creates an
 *   internal direct-key signer automatically.
 *
 * All Nostr primitives (events, keys, encryption, relay I/O) are
 * provided by nostrc (github.com/chebizarro/nostrc).
 */
#include "nostr.h"
#include <nostr-connection.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <json.h>
#include <cjson/cJSON.h>
#include "platform.h"
#include <go.h>
#include <nostr-kinds.h>
#include "log.h"
#include "md_time.h"
#define MD_LOG_TAG "nostr"

/* Forward declaration — defined after DM helpers, used by auth handler */
static NostrEvent *sign_event_via_signer(MdSigner *signer, NostrEvent *ev);
static NostrEvent *sign_event_cancellable(MdSigner *signer, NostrEvent *ev,
                                          const MdSignerCancel *cancel);
static int publish_signed_json(MdNostr *n, const char *json);
static bool list_append_copy(NostrList *list, const char *tag,
                             const char *value, const char *extra);

/* Helper: parse JSON into a new NostrEvent (replaces old nostr_event_from_json) */
static NostrEvent *event_from_json(const char *json) {
    if (!json) return NULL;
    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;
    if (nostr_event_deserialize(ev, json) != 0) {
        nostr_event_free(ev);
        return NULL;
    }
    return ev;
}

/* How a relay last ended the subscription, from the CLOSED reason's NIP-01
 * machine-readable prefix (md-ip2k). Reset by EOSE and by every connection
 * change, so it only ever describes the current connection. */
typedef enum {
    MD_RELAY_CLOSED_NONE = 0, /* no CLOSED since the last EOSE/connection change */
    MD_RELAY_CLOSED_RETRY,    /* "rate-limited:": the relay asks for a later retry */
    MD_RELAY_CLOSED_AUTH,     /* "auth-required:": refused only with no AUTH path */
    MD_RELAY_CLOSED_REFUSED,  /* any other or no prefix: restricted:, blocked:, ... */
} MdRelayClosed;

typedef struct {
    MdNostr *bridge;
    size_t index;
    bool history_done, history_ok; /* allowlist_mu */
    MdRelayClosed closed;          /* allowlist_mu */
    char closed_reason[160];       /* allowlist_mu, for operator logs */
    NostrRelayConnectionState connection_state;
    uint64_t connection_generation; /* allowlist_mu */
    NostrPoolCallbackInfo boundary; /* allowlist_mu: last EOSE/CLOSED in wire order */
} MdRelayState;

/* Failed/conflicting local publications must not become grants via an echo.
 * Bound retained unconfirmed ids; refuse further edits rather than evict a
 * guard. Successful edits release their slot. */
#define MD_ACL_UNCONFIRMED_MAX 256

struct MdNostr {
    bool quality_preferences;
    char *quality_cipher; /* allowlist_mu; bounded encrypted NIP-78 winner */
    int64_t quality_created;
    char quality_id[65];
    bool quality_saving;
    MdAccountBinding  *owner;
    MdAccountWatch    *owner_watch;
    MdSigner          *signer;       /* signing backend                     */
    MdSignerCancel    *signer_cancel; /* bridge-local, never closes shared signer */
    char              *pk_hex;       /* our public key, hex                 */
    NostrSimplePool   *pool;         /* nostrc relay pool — persistent conns */
    NostrList         *allowlist;    /* cached NIP-51 allowlist             */
    cJSON             *acl_private_unknown; /* inert private items, preserved on edit */
    pthread_mutex_t    allowlist_mu;
    pthread_cond_t     allowlist_cv;
    MdNostrAclState    acl_state;
    bool               acl_open, acl_editing;
    bool               acl_review_required, acl_decode_failed;
    bool               acl_sub_failed; /* no REQ was ever issued: FAILED for good */
    uint64_t           acl_version;
    int64_t            acl_created_at;
    char               acl_id[65];
    char               acl_unconfirmed[MD_ACL_UNCONFIRMED_MAX][65];
    size_t             acl_unconfirmed_count;
    MdNostrCallbacks   cbs;          /* event callbacks                     */

    /* Cached relay snapshot — avoids direct access to pool struct internals.
     * Populated once in md_nostr_create() after ensure_relay, before start.
     * Replace with nostrc accessor APIs when available. */
    NostrRelay       **relays;       /* borrowed relay pointers (not owned) */
    size_t             relay_count;
    MdRelayState      *relay_states;
    unsigned           auth_signers; /* work_mu: registrations/attempts retaining n */
    char             **relay_urls;   /* owned copies of URL strings         */

    pthread_mutex_t    work_mu;
    pthread_cond_t     work_cv;
    bool               closing;
    unsigned           active_ops;
    pthread_t          dm_thread;
    bool               dm_started;
    bool               dm_busy;
    char              *dm_queue[MD_NOSTR_DM_QUEUE_CAPACITY];
    bool               dm_live[MD_NOSTR_DM_QUEUE_CAPACITY];
    size_t             dm_head, dm_count, dm_reserved;
    char               wrap_ids[MD_NOSTR_DM_DEDUP_CAPACITY][65];
    char               rumor_ids[MD_NOSTR_DM_DEDUP_CAPACITY][65];
    /* Whether each id has been delivered live (post-EOSE) yet. */
    bool               wrap_live[MD_NOSTR_DM_DEDUP_CAPACITY];
    bool               rumor_live[MD_NOSTR_DM_DEDUP_CAPACITY];
    size_t             wrap_next, rumor_next;
    void (*test_hook)(MdNostrTestPoint, void *);
    void *test_userdata;
};

void md_nostr_set_test_hook(MdNostr *n,
    void (*hook)(MdNostrTestPoint, void *), void *userdata) {
    if (!n) return;
    pthread_mutex_lock(&n->work_mu);
    n->test_hook = hook;
    n->test_userdata = userdata;
    pthread_mutex_unlock(&n->work_mu);
}

static void test_point(MdNostr *n, MdNostrTestPoint point) {
    pthread_mutex_lock(&n->work_mu);
    void (*hook)(MdNostrTestPoint, void *) = n->test_hook;
    void *userdata = n->test_userdata;
    pthread_mutex_unlock(&n->work_mu);
    if (hook) hook(point, userdata);
}

static bool operation_begin(MdNostr *n) {
    if (!n) return false;
    pthread_mutex_lock(&n->work_mu);
    bool accepted = !n->closing && (!n->owner || md_account_binding_is_current(n->owner));
    if (accepted) n->active_ops++;
    pthread_mutex_unlock(&n->work_mu);
    if (accepted) test_point(n, MD_NOSTR_TEST_ADMITTED);
    return accepted;
}

static bool is_stopped(MdNostr *n) {
    pthread_mutex_lock(&n->work_mu);
    bool stopped = n->closing || (n->owner && !md_account_binding_is_current(n->owner));
    pthread_mutex_unlock(&n->work_mu);
    return stopped;
}

static bool owns_host(MdNostr *n) {
    return n && (!n->owner || md_account_binding_role(n->owner) == MD_ACCOUNT_HOST);
}
static void owner_revoked(void *data) {
    MdNostr *n = data;
    md_nostr_stop(n);
    if (n->cbs.on_owner_revoked) n->cbs.on_owner_revoked(n->cbs.owner_userdata);
}

static void operation_end(MdNostr *n) {
    pthread_mutex_lock(&n->work_mu);
    n->active_ops--;
    pthread_cond_broadcast(&n->work_cv);
    pthread_mutex_unlock(&n->work_mu);
}

/* ── Parallel relay publishing ──────────────────────────────── */
typedef struct {
    NostrRelay  *relay;
    NostrEvent  *event;
    GoWaitGroup *wg;
    bool        *accepted; /* per-relay result slot (owned by publish_all) */
} PublishArg;

static void *publish_one_thread(void *arg) {
    PublishArg *pa = arg;
    /* Wait for the relay's NIP-01 OK so publish success is observed,
     * not assumed. */
    *pa->accepted = nostr_relay_publish_and_wait(pa->relay, pa->event,
                                                 0, NULL);
    go_wait_group_done(pa->wg);
    free(pa);
    return NULL;
}

/* Helper: publish an event to all cached relays in parallel.
 * Returns 0 only when at least one relay acknowledged the event with
 * OK=true; -1 when every relay rejected it or never answered. */
static int publish_all(MdNostr *n, NostrEvent *ev) {
    if (!n || !ev || n->relay_count == 0 || is_stopped(n)) return -1;

    GoWaitGroup wg;
    go_wait_group_init(&wg);

    bool *accepted = calloc(n->relay_count, sizeof(bool));
    if (!accepted) {
        go_wait_group_destroy(&wg);
        return -1;
    }

    for (size_t i = 0; i < n->relay_count; i++) {
        go_wait_group_add(&wg, 1);
        PublishArg *pa = malloc(sizeof(PublishArg));
        if (pa) {
            pa->relay    = n->relays[i];
            pa->event    = ev;
            pa->wg       = &wg;
            pa->accepted = &accepted[i];
            go(publish_one_thread, pa);
        } else {
            go_wait_group_done(&wg);
        }
    }

    go_wait_group_wait(&wg);
    go_wait_group_destroy(&wg);

    size_t ok_count = 0;
    for (size_t i = 0; i < n->relay_count; i++)
        if (accepted[i]) ok_count++;
    free(accepted);

    if (ok_count == 0) {
        MD_LOG_W("publish rejected (or unacknowledged) by all %zu relays", n->relay_count);
        return -1;
    }
    return 0;
}

/* Match nostrc's reason-specific recovery: rate-limited/error back off,
 * auth-required waits for AUTH, everything else waits for reconnect. */
static MdRelayClosed classify_closed(const char *reason) {
    if (reason && !strncmp(reason, "auth-required:", 14)) return MD_RELAY_CLOSED_AUTH;
    if (reason && (!strncmp(reason, "rate-limited:", 13) ||
                   !strncmp(reason, "error:", 6))) return MD_RELAY_CLOSED_RETRY;
    return MD_RELAY_CLOSED_REFUSED;
}

/* The library owns challenge/identity/OK correlation. An accepted AUTH is
 * only a path for a CLOSED received before that acceptance. A post-AUTH
 * refusal is terminal until a new challenge (or connection) opens a path. */
static bool auth_path_open(MdNostr *n, MdRelayState *rs) {
    NostrRelayAuthStatus status;
    if (!nostr_relay_get_auth_status(n->relays[rs->index], &status) || is_stopped(n))
        return false;
    if (rs->boundary.recovery == NOSTR_POOL_RECOVERY_AUTH_REFUSED &&
        status.auth_generation == rs->boundary.frame.auth_generation)
        return false;
    /* AWAIT_AUTH may precede the challenge on the wire. With a registered
     * identity it is pending, not a refusal: closing held streams here would
     * race every relay that sends CLOSED before AUTH on reconnect. */
    return status.state == NOSTR_RELAY_AUTH_STATE_NONE ||
           status.state == NOSTR_RELAY_AUTH_STATE_CHALLENGED ||
           status.state == NOSTR_RELAY_AUTH_STATE_SIGNING ||
           status.state == NOSTR_RELAY_AUTH_STATE_SENT ||
           status.state == NOSTR_RELAY_AUTH_STATE_ACCEPTED;
}

/* The relay definitely will not deliver history on this connection: its last
 * boundary was a terminal CLOSED. Disconnected/backoff relays, rate limits
 * and pending AUTH are transient. Caller holds allowlist_mu. */
static bool relay_refused_locked(MdNostr *n, MdRelayState *rs) {
    return rs->history_done && !rs->history_ok &&
        (rs->closed == MD_RELAY_CLOSED_REFUSED ||
         (rs->closed == MD_RELAY_CLOSED_AUTH && !auth_path_open(n, rs)));
}

static bool history_pending_locked(const MdNostr *n) {
    for (size_t i = 0; i < n->relay_count; i++)
        if (!n->relay_states[i].history_done) return true;
    return false;
}

/* Recompute readiness only at protocol/connection boundaries. A relay that
 * fails supplies no trustworthy empty history. At least one successful EOSE
 * and no pending relays are required to authorize. With no history, the
 * answer is FAILED (definitely denied) only when every relay refused the
 * subscription; while any relay is down, rate-limited or awaiting AUTH it is
 * UNKNOWN, since its reconnect + re-REQ may still deliver history. */
static void update_acl_readiness(MdNostr *n) {
    bool pending = false, ok = false, refused = n->relay_count > 0;
    for (size_t i = 0; i < n->relay_count; i++) {
        MdRelayState *rs = &n->relay_states[i];
        pending |= !rs->history_done;
        ok |= rs->history_ok;
        refused = refused && relay_refused_locked(n, rs);
    }
    n->acl_state = n->acl_open ? MD_NOSTR_ACL_OPEN :
        n->acl_sub_failed ? MD_NOSTR_ACL_FAILED :
        pending ? MD_NOSTR_ACL_UNKNOWN : ok ? MD_NOSTR_ACL_LOADED :
        refused ? MD_NOSTR_ACL_FAILED : MD_NOSTR_ACL_UNKNOWN;
    /* Also wakes md_nostr_wait_history(), which open ACLs still need. */
    pthread_cond_broadcast(&n->allowlist_cv);
}

/* A relay's AUTH path appeared or failed outside a subscription boundary. */
static void readiness_changed(MdNostr *n) {
    if (is_stopped(n)) return;
    pthread_mutex_lock(&n->allowlist_mu);
    update_acl_readiness(n);
    pthread_mutex_unlock(&n->allowlist_mu);
    test_point(n, MD_NOSTR_TEST_ALLOWLIST_BOUNDARY);
}

static void md_nostr_subscription_handler(NostrRelay *relay, const char *state,
                                         const char *reason, void *data) {
    MdNostr *n = data;
    if (is_stopped(n)) return;
    NostrPoolCallbackInfo info;
    /* Queued boundaries from a lost connection must not settle the new one.
     * The receive stamp, not callback delivery order, defines AUTH refusal. */
    if (!nostr_simple_pool_get_callback_info(n->pool, &info)) return;
    test_point(n, MD_NOSTR_TEST_SUBSCRIPTION_QUEUED);
    bool closed = !strcmp(state, "CLOSED"), report = false, refused = false;
    MdRelayClosed kind = closed ? classify_closed(reason) : MD_RELAY_CLOSED_NONE;
    char text[sizeof(((MdRelayState *)0)->closed_reason)];
    snprintf(text, sizeof(text), "%s", !closed ? "" : reason && *reason ? reason : "(no reason)");
    pthread_mutex_lock(&n->allowlist_mu);
    if (info.frame.connection_generation != nostr_relay_get_connection_generation(relay) ||
        nostr_relay_get_connection_state(relay) != NOSTR_RELAY_STATE_CONNECTED) {
        pthread_mutex_unlock(&n->allowlist_mu);
        return;
    }
    for (size_t i = 0; i < n->relay_count; i++) if (n->relays[i] == relay) {
        MdRelayState *rs = &n->relay_states[i];
        if (info.frame.sequence <= rs->boundary.frame.sequence) continue;
        rs->boundary = info;
        rs->connection_generation = info.frame.connection_generation;
        rs->connection_state = NOSTR_RELAY_STATE_CONNECTED;
        rs->history_done = true;
        rs->history_ok = !strcmp(state, "EOSE");
        /* Repeated reasons on this connection need only one operator log. */
        report = closed && (rs->closed != kind || strcmp(rs->closed_reason, text));
        rs->closed = kind;
        memcpy(rs->closed_reason, text, sizeof(text));
        refused = relay_refused_locked(n, rs);
    }
    update_acl_readiness(n);
    pthread_mutex_unlock(&n->allowlist_mu);
    if (report)
        MD_LOG_W("subscription CLOSED on %s: %s (%s)", nostr_relay_get_url_const(relay), text,
                 kind == MD_RELAY_CLOSED_RETRY ? "rate-limited: transient, the pool retries" :
                 !refused ? "awaiting NIP-42 AUTH: transient" :
                 kind == MD_RELAY_CLOSED_AUTH ? "auth-required with no usable NIP-42 AUTH: refused" :
                 "refused: no history from this relay until it serves the REQ again");
    test_point(n, MD_NOSTR_TEST_ALLOWLIST_BOUNDARY);
}

static void md_nostr_relay_state_handler(NostrRelay *relay,
    NostrRelayConnectionState old_state, NostrRelayConnectionState state, void *data) {
    (void)old_state; (void)state;
    MdRelayState *rs = data;
    MdNostr *n = rs->bridge;
    if (is_stopped(n)) return;
    pthread_mutex_lock(&n->allowlist_mu);
    /* Read the current state, not a possibly superseded notification. */
    NostrRelayConnectionState current = nostr_relay_get_connection_state(relay);
    uint64_t generation = nostr_relay_get_connection_generation(relay);
    if (current == rs->connection_state && generation == rs->connection_generation) {
        pthread_mutex_unlock(&n->allowlist_mu);
        return;
    }
    rs->connection_state = current;
    rs->connection_generation = generation;
    memset(&rs->boundary, 0, sizeof(rs->boundary));
    rs->history_done = current == NOSTR_RELAY_STATE_DISCONNECTED ||
                       current == NOSTR_RELAY_STATE_BACKOFF;
    rs->history_ok = false;
    /* A refusal belongs to the connection that answered it: loss is transient. */
    rs->closed = MD_RELAY_CLOSED_NONE;
    rs->closed_reason[0] = '\0';
    update_acl_readiness(n);
    pthread_mutex_unlock(&n->allowlist_mu);
    test_point(n, MD_NOSTR_TEST_ALLOWLIST_BOUNDARY);
}

/* nostrc runs this on an attempt-owned signer thread, never the reader.
 * The registration retains the bridge until every attempt has returned.
 * Bridge/role cancellation discards non-abortable signer IPC; nostrc also
 * cancels attempts on challenge/connection/identity changes and verifies the
 * signed template (including its nonce) before admission to the writer. */
static void md_nostr_auth_sign(NostrRelayAuthRequest *request, void *data) {
    MdNostr *n = data;
    if (is_stopped(n) || nostr_relay_auth_request_is_cancelled(request)) {
        nostr_relay_auth_request_complete(request, NULL, "host role stopped");
        return;
    }
    char *json = nostr_event_serialize(nostr_relay_auth_request_get_event(request));
    char *signed_json = NULL;
    int result = json ? md_signer_sign_event_cancellable(n->signer, json,
                            &signed_json, n->signer_cancel) : MD_SIGNER_ERR_DENIED;
    free(json);
    nostr_relay_auth_request_complete_json(request,
        result == MD_SIGNER_OK && !is_stopped(n) ? signed_json : NULL,
        "host AUTH signing failed or cancelled");
    free(signed_json);
}

static void md_nostr_auth_signer_release(void *data) {
    MdNostr *n = data;
    pthread_mutex_lock(&n->work_mu);
    n->auth_signers--;
    pthread_cond_broadcast(&n->work_cv);
    pthread_mutex_unlock(&n->work_mu);
}

/* Challenge observation only: signing and recovery belong to nostrc. */
static void md_nostr_auth_handler(NostrRelay *relay, const char *challenge, void *data) {
    (void)relay; (void)challenge;
    readiness_changed(((MdRelayState *)data)->bridge);
}

static void md_nostr_auth_result(NostrRelay *relay,
                                 const NostrRelayAuthResult *result, void *data) {
    MdRelayState *rs = data;
    MdNostr *n = rs->bridge;
    uint64_t identity = n->owner ? md_account_binding_generation(n->owner) : 1;
    NostrRelayAuthStatus status;
    if (is_stopped(n) || result->identity_generation != identity ||
        !nostr_relay_get_auth_status(relay, &status) ||
        result->frame.connection_generation != status.connection_generation ||
        result->frame.auth_generation != status.auth_generation)
        return;
    if (result->state == NOSTR_RELAY_AUTH_STATE_ACCEPTED)
        MD_LOG_I("AUTH accepted by %s", n->relay_urls[rs->index]);
    else
        MD_LOG_W("AUTH %s by %s: %s", nostr_relay_auth_state_name(result->state),
                 n->relay_urls[rs->index], result->reason);
    readiness_changed(n);
    test_point(n, MD_NOSTR_TEST_AUTH_RESULT);
}

/* ── Relay OK callback ─────────────────────────────────────────
 * Fires on the relay worker thread when a relay sends an OK
 * response (["OK","<event_id>",true/false,"<reason>"]) after we
 * publish an event.  We log rejections and forward to the
 * caller-provided callback if one is registered.
 */
static void md_nostr_ok_handler(const char *event_id, bool ok,
                                const char *reason, void *user_data) {
    MdRelayState *rs = user_data;
    MdNostr *n = rs->bridge;
    if (!n || is_stopped(n)) return;
    if (!ok) {
        MD_LOG_W("relay REJECTED event %.16s%s — %s", event_id ? event_id : "(null)", event_id && strlen(event_id) > 16 ? "..." : "", reason ? reason : "(no reason)");
    }

    if (n->cbs.on_publish_result) {
        n->cbs.on_publish_result(event_id, ok, reason,
                                 n->cbs.publish_result_userdata);
    }
}

/* One worker owns the replay windows; relay threads only enqueue bounded
 * copies. No signer IPC or application callback runs under work_mu. */
const char *md_nostr_dm_result_name(MdNostrDmResult result) {
    static const char *names[] = {
        "ok", "invalid-wrap", "wrong-recipient", "too-large", "decrypt-failed",
        "invalid-seal", "sender-mismatch", "invalid-rumor", "duplicate",
        "queue-full", "stopped", "no-memory"
    };
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "unknown";
}

static int recent_id(char ids[MD_NOSTR_DM_DEDUP_CAPACITY][65], const char *id) {
    for (size_t i = 0; i < MD_NOSTR_DM_DEDUP_CAPACITY; i++)
        if (strcmp(ids[i], id) == 0) return (int)i;
    return -1;
}

/* md-6ns: an id seen only as history (e.g. one relay's catch-up) is delivered
 * again, once, when its first live copy arrives from any relay; real-time
 * consumers ignore history. Every other repeat is a duplicate. */
static bool seen_before(char ids[MD_NOSTR_DM_DEDUP_CAPACITY][65],
                        const bool delivered_live[MD_NOSTR_DM_DEDUP_CAPACITY],
                        const char *id, bool live, int *slot) {
    *slot = recent_id(ids, id);
    return *slot >= 0 && (delivered_live[*slot] || !live);
}
static void remember_id(char ids[MD_NOSTR_DM_DEDUP_CAPACITY][65],
                        bool delivered_live[MD_NOSTR_DM_DEDUP_CAPACITY],
                        size_t *next, int slot, const char *id, bool live) {
    if (slot < 0) {
        slot = (int)((*next)++ % MD_NOSTR_DM_DEDUP_CAPACITY);
        memcpy(ids[slot], id, 65);
        delivered_live[slot] = false;
    }
    delivered_live[slot] |= live;
}

static bool addressed_to(NostrEvent *ev, const char *pk) {
    NostrTags *tags = nostr_event_get_tags(ev);
    size_t recipients = 0;
    for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_size(tag) < 1 || strcmp(nostr_tag_get(tag, 0), "p") != 0)
            continue;
        if (nostr_tag_size(tag) < 2 || strcmp(nostr_tag_get(tag, 1), pk) != 0)
            return false; /* metadesk session control is one-to-one */
        recipients++;
    }
    return recipients == 1;
}

static MdNostrDmResult unwrap_dm(MdNostr *n, NostrEvent *wrap, bool live) {
    MdNostrDmResult result = MD_NOSTR_DM_INVALID_WRAP;
    NostrEvent *seal = NULL, *rumor = NULL;
    char *seal_json = NULL, *rumor_json = NULL;
    char wrap_id[65], rumor_id[65];
    int wrap_slot = -1, rumor_slot = -1;
    if (!nostr_nip17_validate_gift_wrap(wrap) ||
        nostr_event_validate(wrap, wrap_id) != NOSTR_EVENT_VALIDATION_OK)
        goto done;
    result = MD_NOSTR_DM_WRONG_RECIPIENT;
    if (!addressed_to(wrap, n->pk_hex)) goto done;
    result = MD_NOSTR_DM_DUPLICATE;
    if (seen_before(n->wrap_ids, n->wrap_live, wrap_id, live, &wrap_slot)) goto done;
    result = MD_NOSTR_DM_DECRYPT_FAILED;
    if (md_signer_nip44_decrypt_cancellable(n->signer, nostr_event_get_pubkey(wrap),
                              nostr_event_get_content(wrap), &seal_json, n->signer_cancel) != 0)
        goto done;
    result = MD_NOSTR_DM_STOPPED;
    if (is_stopped(n)) goto done;
    result = MD_NOSTR_DM_TOO_LARGE;
    if (strlen(seal_json) > MD_NOSTR_DM_MAX_BYTES) goto done;
    seal = event_from_json(seal_json);
    result = MD_NOSTR_DM_INVALID_SEAL;
    if (!seal || !nostr_nip17_validate_seal(seal, NULL) ||
        nostr_tags_size(nostr_event_get_tags(seal)) != 0) goto done;
    result = MD_NOSTR_DM_DECRYPT_FAILED;
    if (md_signer_nip44_decrypt_cancellable(n->signer, nostr_event_get_pubkey(seal),
                              nostr_event_get_content(seal), &rumor_json, n->signer_cancel) != 0)
        goto done;
    result = MD_NOSTR_DM_STOPPED;
    if (is_stopped(n)) goto done;
    result = MD_NOSTR_DM_TOO_LARGE;
    if (strlen(rumor_json) > MD_NOSTR_DM_MAX_BYTES) goto done;
    rumor = event_from_json(rumor_json);
    result = MD_NOSTR_DM_INVALID_RUMOR;
    if (!rumor || nostr_event_get_kind(rumor) != NOSTR_KIND_DIRECT_MESSAGE ||
        (rumor->sig && rumor->sig[0]) || !nostr_event_get_content(rumor) ||
        nostr_event_validate_id(rumor, rumor_id) != NOSTR_EVENT_VALIDATION_OK)
        goto done;
    result = MD_NOSTR_DM_SENDER_MISMATCH;
    if (!nostr_nip17_validate_seal(seal, rumor)) goto done;
    result = MD_NOSTR_DM_WRONG_RECIPIENT;
    if (!addressed_to(rumor, n->pk_hex)) goto done;
    result = MD_NOSTR_DM_DUPLICATE;
    if (seen_before(n->rumor_ids, n->rumor_live, rumor_id, live, &rumor_slot)) goto done;
    /* Never use the randomized seal/wrap timestamps for freshness. */
    remember_id(n->wrap_ids, n->wrap_live, &n->wrap_next, wrap_slot, wrap_id, live);
    remember_id(n->rumor_ids, n->rumor_live, &n->rumor_next, rumor_slot, rumor_id, live);
    result = MD_NOSTR_DM_STOPPED;
    if (is_stopped(n)) goto done;
    MdNostrDmInfo info = {
        .rumor_created_at = nostr_event_get_created_at(rumor),
        .live = live,
    };
    n->cbs.on_dm(nostr_event_get_pubkey(rumor), nostr_event_get_content(rumor),
                 &info, n->cbs.dm_userdata);
    result = MD_NOSTR_DM_OK;
done:
    free(seal_json); free(rumor_json);
    if (seal) nostr_event_free(seal);
    if (rumor) nostr_event_free(rumor);
    return result;
}

static void *dm_worker(void *arg) {
    MdNostr *n = arg;
    pthread_mutex_lock(&n->work_mu);
    for (;;) {
        while (!n->closing && n->dm_count == 0)
            pthread_cond_wait(&n->work_cv, &n->work_mu);
        if (n->closing) {
            while (n->dm_count) {
                free(n->dm_queue[n->dm_head]);
                n->dm_head = (n->dm_head + 1) % MD_NOSTR_DM_QUEUE_CAPACITY;
                n->dm_count--;
            }
            pthread_cond_broadcast(&n->work_cv);
            break;
        }
        char *json = n->dm_queue[n->dm_head];
        bool live = n->dm_live[n->dm_head];
        n->dm_head = (n->dm_head + 1) % MD_NOSTR_DM_QUEUE_CAPACITY;
        n->dm_count--;
        n->dm_busy = true;
        pthread_mutex_unlock(&n->work_mu);
        NostrEvent *wrap = event_from_json(json);
        free(json);
        MdNostrDmResult result = wrap ? unwrap_dm(n, wrap, live) : MD_NOSTR_DM_INVALID_WRAP;
        if (wrap) nostr_event_free(wrap);
        if (result != MD_NOSTR_DM_OK && !is_stopped(n)) {
            MD_LOG_W("DM rejected: %s", md_nostr_dm_result_name(result));
            if (n->cbs.on_dm_error) n->cbs.on_dm_error(result, n->cbs.dm_userdata);
        }
        pthread_mutex_lock(&n->work_mu);
        n->dm_busy = false;
        pthread_cond_broadcast(&n->work_cv);
    }
    pthread_mutex_unlock(&n->work_mu);
    return NULL;
}

/* Bound JSON escaping and container overhead without allocating. The copy
 * contains only NIP-01 fields; provider-specific extra data is not forwarded. */
static bool json_string_fits(const char *s, size_t *budget) {
    if (!s) return true;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        size_t cost = *p < 0x20 ? 6 : (*p == '"' || *p == '\\') ? 2 : 1;
        if (*budget < cost) return false;
        *budget -= cost;
    }
    return true;
}
static bool dm_event_fits(const NostrEvent *ev) {
    size_t budget = MD_NOSTR_DM_MAX_BYTES - 512; /* keys, punctuation, integers */
    if (!json_string_fits(ev->id, &budget) || !json_string_fits(ev->pubkey, &budget) ||
        !json_string_fits(ev->sig, &budget) || !json_string_fits(ev->content, &budget))
        return false;
    for (size_t i = 0; i < nostr_tags_size(ev->tags); i++) {
        if (budget < 3) return false;
        budget -= 3; /* tag array and comma */
        NostrTag *tag = nostr_tags_get(ev->tags, i);
        for (size_t j = 0; j < nostr_tag_size(tag); j++) {
            if (budget < 3) return false;
            budget -= 3; /* quotes and comma */
            if (!json_string_fits(nostr_tag_get(tag, j), &budget)) return false;
        }
    }
    return true;
}

MdNostrDmResult md_nostr_receive_dm(MdNostr *n, NostrEvent *wrap) {
    if (!operation_begin(n)) return MD_NOSTR_DM_STOPPED;
    MdNostrDmResult result = MD_NOSTR_DM_INVALID_WRAP;
    char *json = NULL;
    bool reserved = false;
    if (!wrap || !n->cbs.on_dm) goto done;
    /* Ordered post-EOSE fact for this delivery; zero (history) outside the
     * pool's single-event callback, e.g. for direct test ingress. */
    bool live = nostr_simple_pool_get_callback_live_since(n->pool) > 0;
    pthread_mutex_lock(&n->work_mu);
    if (n->closing) result = MD_NOSTR_DM_STOPPED;
    else if (n->dm_count + n->dm_reserved >= MD_NOSTR_DM_QUEUE_CAPACITY)
        result = MD_NOSTR_DM_QUEUE_FULL;
    else { n->dm_reserved++; reserved = true; }
    pthread_mutex_unlock(&n->work_mu);
    if (!reserved) goto done;
    result = MD_NOSTR_DM_TOO_LARGE;
    if (!dm_event_fits(wrap)) goto done;
    NostrEvent copy = *wrap;
    copy.extra = NULL;
    test_point(n, MD_NOSTR_TEST_SERIALIZING);
    json = nostr_event_serialize(&copy);
    result = MD_NOSTR_DM_NO_MEMORY;
    if (!json) goto done;
    result = MD_NOSTR_DM_TOO_LARGE;
    if (strlen(json) > MD_NOSTR_DM_MAX_BYTES) goto done;
    pthread_mutex_lock(&n->work_mu);
    if (n->closing) result = MD_NOSTR_DM_STOPPED;
    else {
        n->dm_reserved--;
        reserved = false;
        size_t slot = (n->dm_head + n->dm_count++) % MD_NOSTR_DM_QUEUE_CAPACITY;
        n->dm_queue[slot] = json;
        n->dm_live[slot] = live;
        json = NULL;
        result = MD_NOSTR_DM_OK;
        pthread_cond_broadcast(&n->work_cv);
    }
    pthread_mutex_unlock(&n->work_mu);
done:
    free(json);
    if (reserved) {
        pthread_mutex_lock(&n->work_mu);
        n->dm_reserved--;
        pthread_cond_broadcast(&n->work_cv);
        pthread_mutex_unlock(&n->work_mu);
    }
    operation_end(n);
    return result;
}

void md_nostr_drain_dm(MdNostr *n) {
    if (!operation_begin(n)) return;
    pthread_mutex_lock(&n->work_mu);
    while (n->dm_count || n->dm_busy || n->dm_reserved)
        pthread_cond_wait(&n->work_cv, &n->work_mu);
    pthread_mutex_unlock(&n->work_mu);
    operation_end(n);
}

int md_nostr_wait_history(MdNostr *n, uint32_t timeout_ms) {
    if (!operation_begin(n)) return -1;
    /* md-l4f4: a wall-clock step must neither extend nor cut short the wait. */
    const uint64_t deadline = md_monotonic_ns() + (uint64_t)timeout_ms * 1000000u;
    pthread_mutex_lock(&n->allowlist_mu);
    bool pending = true, ok = false, expired = false;
    while (!is_stopped(n)) {
        pending = ok = false;
        for (size_t i = 0; i < n->relay_count; i++) {
            pending |= !n->relay_states[i].history_done;
            ok |= n->relay_states[i].history_ok;
        }
        if (!pending || expired) break;
        /* EOSE/CLOSED/connection-state callbacks broadcast; no polling.
         * Fired with allowlist_mu held: any boundary the test causes next
         * needs the lock, so it lands while this thread is in the wait. */
        test_point(n, MD_NOSTR_TEST_HISTORY_WAITING);
        expired = md_cond_timedwait_until(&n->allowlist_cv, &n->allowlist_mu,
                                          deadline) != 0;
    }
    pthread_mutex_unlock(&n->allowlist_mu);
    int ret = !pending && ok && !is_stopped(n) ? 0 : -1;
    operation_end(n);
    return ret;
}

/* ── Allowlist decoding ───────────────────────────────────────── */

/* Append a copy, leaving the list intact on failure. Deliberately not
 * nostr_nip51_list_add_entry(): its failure contract differs by nostrc version
 * (legacy keeps the list and leaks the entry; >= 5d1283a7c frees the entry
 * and empties the list), so no caller can clean up correctly for both. The
 * NostrList layout and its free() ownership are identical in both. */
static bool list_append_copy(NostrList *list, const char *tag,
                             const char *value, const char *extra) {
    if (!list || (!list->entries && list->capacity)) return false;
    if (list->count >= list->capacity) {
        size_t cap = list->capacity ? list->capacity * 2 : 8;
        if (cap > SIZE_MAX / sizeof(*list->entries)) return false;
        NostrListEntry **next = realloc(list->entries, cap * sizeof(*next));
        if (!next) return false;
        list->entries = next;
        list->capacity = cap;
    }
    NostrListEntry *entry = nostr_nip51_entry_new(tag, value, extra, false);
    if (!entry) return false;
    list->entries[list->count++] = entry;
    return true;
}

/* Both public tags and decrypted NIP-51 items use the md-jqnz raw-tag
 * policy. Wrong-arity grants stay invalid across unrelated edits. */
static bool allowlist_item(NostrList *list, const char *name, const char *value,
                           const char *third, size_t arity) {
    if (!name || !value) return true;
    if (!strcmp(name, "p")) return list_append_copy(list, name, value, third);
    if (!strcmp(name, "metadesk-caps"))
        return list_append_copy(list, name, value, arity == 3 && third ? third : "");
    return true;
}

/* cJSON exposes NUL-terminated values, so a JSON U+0000 escape could turn
 * "v1:video\\u0000junk" into a valid grant. Skip escaped backslashes correctly;
 * reject U+0000 before any JSON field crosses into a C-string policy API. */
static bool allowlist_json_strings_safe(const char *json) {
    for (const char *p = json; *p; p++) {
        if (*p != '\\') continue;
        if (!*++p) break;
        if (*p == 'u' && !strncmp(p + 1, "0000", 4)) return false;
    }
    return true;
}

#define MD_ALLOWLIST_MAX_CONTENT 131072

/* Envelope author/id/signature are checked before this function. A valid newer
 * envelope with unreadable content replaces old authority with denial, not a
 * fallback to its public tags or an older grant. Only a foreign address is ignored. */
static bool allowlist_from_event(MdNostr *n, NostrEvent *ev, NostrList **out,
                                 bool *review, bool *failed, cJSON **unknown_out) {
    *out = NULL;
    *unknown_out = NULL;
    *review = *failed = false;
    NostrTags *tags = nostr_event_get_tags(ev);
    size_t count = tags ? nostr_tags_size(tags) : 0;
    const char *identifier = NULL;
    for (size_t i = 0; i < count; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        const char *name = tag && nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 0) : NULL;
        if (name && !strcmp(name, "d")) { identifier = nostr_tag_get(tag, 1); break; }
    }
    if (!identifier || strcmp(identifier, "metadesk-allowlist")) return false;
    NostrList *list = nostr_nip51_list_new();
    bool ok = list != NULL;
    if (ok) {
        nostr_nip51_list_set_identifier(list, identifier);
        ok = list->identifier != NULL;
    }
    for (size_t i = 0; ok && i < count; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        size_t arity = tag ? nostr_tag_size(tag) : 0;
        const char *name = arity >= 2 ? nostr_tag_get(tag, 0) : NULL;
        const char *value = name ? nostr_tag_get(tag, 1) : NULL;
        const char *third = arity >= 3 ? nostr_tag_get(tag, 2) : NULL;
        if (name && (!strcmp(name, "p") || !strcmp(name, "metadesk-caps")))
            *review = true; /* Mixed lists are review-only as a whole. */
        ok = allowlist_item(list, name, value, third, arity);
    }
    const char *content = nostr_event_get_content(ev);
    char *plain = NULL;
    cJSON *items = NULL, *unknown = NULL;
    if (ok && content && *content) {
        ok = strlen(content) <= MD_ALLOWLIST_MAX_CONTENT &&
            md_signer_nip44_decrypt_cancellable(n->signer, n->pk_hex, content,
                                               &plain, n->signer_cancel) == 0 && plain;
        if (ok) {
            ok = strlen(plain) <= MD_ALLOWLIST_MAX_CONTENT && allowlist_json_strings_safe(plain);
            if (ok) items = cJSON_ParseWithOpts(plain, NULL, true);
            unknown = cJSON_CreateArray();
            ok = cJSON_IsArray(items) && unknown;
        }
        cJSON *item;
        cJSON_ArrayForEach(item, items) {
            if (!ok) break;
            ok = cJSON_IsArray(item);
            cJSON *field;
            cJSON_ArrayForEach(field, item) if (!cJSON_IsString(field)) ok = false;
            if (!ok) break;
            size_t arity = (size_t)cJSON_GetArraySize(item);
            const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(item, 0));
            const char *value = cJSON_GetStringValue(cJSON_GetArrayItem(item, 1));
            const char *third = cJSON_GetStringValue(cJSON_GetArrayItem(item, 2));
            ok = allowlist_item(list, name, value, third, arity);
            if (ok && (!name || (strcmp(name, "p") && strcmp(name, "metadesk-caps")))) {
                cJSON *copy = cJSON_Duplicate(item, true);
                if (!copy || !cJSON_AddItemToArray(unknown, copy)) {
                    cJSON_Delete(copy); ok = false;
                }
            }
        }
    }
    if (plain) { md_secure_zero(plain, strlen(plain)); free(plain); }
    cJSON_Delete(items);
    if (!ok) {
        MD_LOG_W("allowlist private content unavailable/invalid; denying access");
        nostr_nip51_list_free(list);
        list = NULL;
        *failed = true;
        cJSON_Delete(unknown); unknown = NULL;
    }
    *unknown_out = unknown;
    *out = list;
    return true;
}

/* ── Event middleware ─────────────────────────────────────────
 * nostrc's NostrSimplePool invokes the event_middleware callback
 * for every incoming event across all subscriptions. We route
 * events to the appropriate metadesk callback based on kind.
 *
 * This runs on the pool's worker thread — callbacks must be
 * thread-safe or dispatch to the main thread.
 */
static void handle_incoming_event(NostrIncomingEvent *incoming,
                                  void *user_data) {
    if (!incoming || !incoming->event)
        return;

    MdNostr *n = user_data;
    if (!n || is_stopped(n))
        return;
    NostrEvent *ev = incoming->event;

    int kind = nostr_event_get_kind(ev);

    if (kind == 30078 && n->quality_preferences) {
        const char *author = nostr_event_get_pubkey(ev), *content = nostr_event_get_content(ev);
        char id[65];
        if (!author || strcmp(author, n->pk_hex) || !content || strlen(content) > 65536 ||
            nostr_event_validate(ev, id) != NOSTR_EVENT_VALIDATION_OK ||
            nostr_event_get_created_at(ev) > (int64_t)time(NULL) + 30) return;
        NostrTags *tags = nostr_event_get_tags(ev);
        unsigned matches = 0, ds = 0;
        for (size_t i=0; tags && i<nostr_tags_size(tags); i++) {
            NostrTag *tag = nostr_tags_get(tags, i);
            const char *k = tag && nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 0) : NULL;
            if (k && !strcmp(k, "d")) {
                ds++; const char *v = nostr_tag_get(tag, 1);
                if (v && !strcmp(v, "metadesk-session-preferences")) matches++;
            }
        }
        if (ds != 1 || matches != 1) return;
        pthread_mutex_lock(&n->allowlist_mu);
        int64_t created = nostr_event_get_created_at(ev);
        if (created > n->quality_created || (created == n->quality_created &&
            (!n->quality_id[0] || strcmp(id, n->quality_id) < 0))) {
            char *copy = strdup(content);
            if (copy) {
                free(n->quality_cipher); n->quality_cipher = copy;
                n->quality_created = created; strcpy(n->quality_id, id);
            }
        }
        pthread_mutex_unlock(&n->allowlist_mu);
    } else if (kind == NOSTR_KIND_GIFT_WRAP && n->cbs.on_dm && n->signer) {
        MdNostrDmResult result = md_nostr_receive_dm(n, ev);
        if (result != MD_NOSTR_DM_OK)
            MD_LOG_W("DM ingress rejected: %s", md_nostr_dm_result_name(result));
    } else if (kind == NOSTR_KIND_CATEGORIZED_PEOPLE_LIST) {
        if (!owns_host(n)) return;
        /* NIP-51 categorized people list — check if it's our allowlist.
         * The list is access control: only accept it from our own key
         * with a valid signature. The REQ filter is enforced by the
         * relay, which is not an authorization boundary. */
        const char *author = nostr_event_get_pubkey(ev);
        if (!author || !n->pk_hex || strcmp(author, n->pk_hex) != 0)
            return;
        char canon[65];
        if (nostr_event_validate(ev, canon) != NOSTR_EVENT_VALIDATION_OK) {
            MD_LOG_W("rejected allowlist event with invalid id/sig");
            return;
        }

        if (nostr_event_get_created_at(ev) > (int64_t)time(NULL) + 300) return;
        NostrList *list = NULL;
        bool review = false, failed = false;
        cJSON *unknown = NULL;
        if (allowlist_from_event(n, ev, &list, &review, &failed, &unknown)) {
            test_point(n, MD_NOSTR_TEST_ALLOWLIST_PROCESSING);
            pthread_mutex_lock(&n->allowlist_mu);
            bool pending = false;
            for (size_t i = 0; i < n->acl_unconfirmed_count; i++)
                if (!strcmp(n->acl_unconfirmed[i], canon)) pending = true;
            int64_t created = nostr_event_get_created_at(ev);
            bool newer = !n->acl_id[0] || created > n->acl_created_at ||
                (created == n->acl_created_at && strcmp(canon, n->acl_id) < 0);
            if (newer && !pending) {
                nostr_nip51_list_free(n->allowlist);
                n->allowlist = list;
                cJSON_Delete(n->acl_private_unknown);
                n->acl_private_unknown = unknown; unknown = NULL;
                n->acl_review_required = review;
                n->acl_decode_failed = failed;
                list = NULL;
                n->acl_created_at = created;
                memcpy(n->acl_id, canon, sizeof(n->acl_id));
                n->acl_version++;
                pthread_cond_broadcast(&n->allowlist_cv);
            }
            pthread_mutex_unlock(&n->allowlist_mu);
            nostr_nip51_list_free(list);
            cJSON_Delete(unknown);
            test_point(n, MD_NOSTR_TEST_ALLOWLIST_RECEIVED);
        } else {
            test_point(n, MD_NOSTR_TEST_ALLOWLIST_IGNORED);
        }
    }
}

/* Pool middleware owns the event even on rejection/stopped paths. DM ingress
 * serializes its bounded copy; NIP-51 parsing copies entries, not the event. */
static void md_nostr_event_handler(NostrIncomingEvent *incoming, void *user_data) {
    if (!incoming) return;
    handle_incoming_event(incoming, user_data);
    nostr_event_free(incoming->event);
}

/* ── Lifecycle ────────────────────────────────────────────── */

/* Subscribe once with the full merged filter set: live NIP-17 DM
 * subscription plus our own NIP-51 allowlist (kind:30000). nostrc
 * replaces the pool's shared filter set on every subscribe call while
 * subscriptions borrow it, so all filters must go out in a single REQ
 * batch — see md_nostr_refresh_allowlist(). */
static int md_nostr_subscribe(MdNostr *n) {
    NostrFilters *filters = nostr_filters_new();
    if (!filters) return -1;

    size_t added = 0;

    /* kind:1059 gift-wraps addressed to us.
     * NIP-59 randomizes timestamps up to two days into the past. Include
     * that full window plus reconnect slack, and bound relay backfill. */
    if (n->cbs.on_dm) {
        NostrFilter *f = nostr_filter_builder_build(
            nostr_filter_builder_limit(nostr_filter_builder_since(
                nostr_filter_builder_tag(
                    nostr_filter_builder_kinds(
                        nostr_filter_builder_new(),
                        NOSTR_KIND_GIFT_WRAP, -1),
                    "p", n->pk_hex),
                (int64_t)time(NULL) - (2 * 86400 + 300)),
                MD_NOSTR_DM_DEDUP_CAPACITY));
        if (f) {
            nostr_filters_add(filters, f);
            nostr_filter_free(f);
            added++;
        }
    }

    /* kind:30000, authors:[our_pk], #d:["metadesk-allowlist"].
     * limit:1 — addressable event (NIP-33), only the latest exists;
     * the subscription stays open for live replacements. */
    if (owns_host(n)) {
        NostrFilter *f = nostr_filter_builder_build(
            nostr_filter_builder_limit(
                nostr_filter_builder_tag(
                    nostr_filter_builder_authors(
                        nostr_filter_builder_kinds(
                            nostr_filter_builder_new(),
                            NOSTR_KIND_CATEGORIZED_PEOPLE_LIST, -1),
                        n->pk_hex, NULL),
                    "d", "metadesk-allowlist"),
                1));
        if (f) {
            nostr_filters_add(filters, f);
            nostr_filter_free(f);
            added++;
        }
    }

    if (n->quality_preferences) {
        NostrFilter *f = nostr_filter_builder_build(nostr_filter_builder_limit(
            nostr_filter_builder_tag(nostr_filter_builder_authors(nostr_filter_builder_kinds(
                nostr_filter_builder_new(), 30078, -1), n->pk_hex, NULL),
                "d", "metadesk-session-preferences"), 1));
        if (f) { nostr_filters_add(filters, f); nostr_filter_free(f); added++; }
    }
    if (added > 0)
        nostr_simple_pool_subscribe(n->pool, (const char **)n->relay_urls,
                                    n->relay_count, *filters, false);
    nostr_filters_free(filters);
    return added > 0 ? 0 : -1;
}

MdNostr *md_nostr_create(const MdNostrConfig *cfg, const MdNostrCallbacks *cbs) {
    if (!cfg || !cfg->relay_urls || cfg->relay_count <= 0)
        return NULL;

    /* Must have either signer or sk_hex */
    if (!cfg->signer && !cfg->sk_hex && !cfg->owner)
        return NULL;
    if (cfg->owner && (!md_account_binding_is_current(cfg->owner) ||
        (cfg->signer && cfg->signer != md_account_binding_signer(cfg->owner))))
        return NULL;

    MdNostr *n = calloc(1, sizeof(MdNostr));
    if (!n) return NULL;

    pthread_mutex_init(&n->allowlist_mu, NULL);
    /* md-l4f4: monotonic, for md_nostr_wait_history's bounded wait; every
     * other wait on it is untimed. */
    md_cond_init_monotonic(&n->allowlist_cv);
    pthread_mutex_init(&n->work_mu, NULL);
    pthread_cond_init(&n->work_cv, NULL);
    n->quality_preferences = cfg->quality_preferences;
    n->acl_open = cfg->allowlist_open;
    n->acl_state = cfg->allowlist_open ? MD_NOSTR_ACL_OPEN : MD_NOSTR_ACL_UNKNOWN;
    n->signer_cancel = md_signer_cancel_new();
    if (!n->signer_cancel) { md_nostr_destroy(n); return NULL; }

    /* Set up signer: use provided signer, or create direct-key from sk_hex */
    n->owner = md_account_binding_ref(cfg->owner);
    if (cfg->owner || cfg->signer) {
        n->signer = md_signer_ref(cfg->owner ? md_account_binding_signer(cfg->owner) : cfg->signer);
    } else {
        n->signer = md_signer_create_direct(cfg->sk_hex);
        if (!n->signer) {
            MD_LOG_E("failed to create signer from sk_hex");
            md_nostr_destroy(n);
            return NULL;
        }
    }

    /* Get pubkey from signer */
    char *pk = NULL;
    if (md_signer_get_pubkey(n->signer, &pk) != MD_SIGNER_OK || !pk) {
        MD_LOG_E("failed to get pubkey from signer");
        md_nostr_destroy(n);
        return NULL;
    }
    n->pk_hex = pk;

    /* Store callbacks */
    if (cbs)
        n->cbs = *cbs;

    /* Create relay pool — persistent WebSocket connections */
    n->pool = nostr_simple_pool_new();
    if (!n->pool) {
        md_nostr_destroy(n);
        return NULL;
    }

    /* Reconnect catch-up must include NIP-59's randomized timestamp range,
     * not a cursor derived from untrusted received timestamps. */
    if (!nostr_simple_pool_set_recovery_window(n->pool, 2 * 86400 + 300)) {
        MD_LOG_E("pool recovery-window configuration failed");
        md_nostr_destroy(n);
        return NULL;
    }

    nostr_simple_pool_set_auto_unsub_on_eose(n->pool, false);
    nostr_simple_pool_set_subscription_callback(n->pool, md_nostr_subscription_handler, n);

    /* Register event middleware for incoming event routing.
     * The _ex variant carries our MdNostr as user_data. */
    nostr_simple_pool_set_event_middleware_ex(n->pool,
                                              md_nostr_event_handler, n);

    /* Add all relay URLs — pool manages reconnection with backoff */
    for (int i = 0; i < cfg->relay_count; i++) {
        nostr_simple_pool_ensure_relay(n->pool, cfg->relay_urls[i]);
    }

    /* Snapshot relay pointers and URLs from pool.
     * This is the ONLY place we access pool struct internals.
     * TODO: Replace with nostr_simple_pool_get_relays() when nostrc
     * provides an accessor API. */
    size_t relay_capacity = (size_t)cfg->relay_count;
    n->relays = calloc(relay_capacity, sizeof(NostrRelay *));
    n->relay_urls = calloc(relay_capacity, sizeof(char *));
    if (n->relays && n->relay_urls) {
        pthread_mutex_lock(&n->pool->pool_mutex);
        for (size_t i = 0; i < relay_capacity && i < n->pool->relay_count; i++) {
            const char *url = nostr_relay_get_url_const(n->pool->relays[i]);
            char *url_copy = strdup(url ? url : "");
            if (!url_copy)
                continue;
            n->relays[n->relay_count] = n->pool->relays[i];
            n->relay_urls[n->relay_count] = url_copy;
            n->relay_count++;
        }
        pthread_mutex_unlock(&n->pool->pool_mutex);

        n->relay_states = calloc(n->relay_count, sizeof(*n->relay_states));
        if (!n->relay_states) { md_nostr_destroy(n); return NULL; }
        /* Register per-relay callbacks.  Both have a user_data parameter
         * (unlike event_middleware), so we pass MdNostr directly. */
        /* All callback data exists before the first callback can run. */
        for (size_t i = 0; i < n->relay_count; i++) {
            n->relay_states[i].bridge = n;
            n->relay_states[i].index = i;
        }
        pthread_mutex_lock(&n->allowlist_mu);
        for (size_t i = 0; i < n->relay_count; i++) {
            nostr_relay_set_state_callback(n->relays[i], md_nostr_relay_state_handler,
                                          &n->relay_states[i]);
            NostrRelayConnectionState state = nostr_relay_get_connection_state(n->relays[i]);
            n->relay_states[i].connection_state = state;
            n->relay_states[i].history_done = state == NOSTR_RELAY_STATE_DISCONNECTED ||
                                            state == NOSTR_RELAY_STATE_BACKOFF;
            nostr_relay_set_ok_callback(n->relays[i],
                                        md_nostr_ok_handler, &n->relay_states[i]);
            nostr_relay_set_auth_result_callback_full(n->relays[i],
                md_nostr_auth_result, &n->relay_states[i], NULL);
            n->relay_states[i].connection_generation =
                nostr_relay_get_connection_generation(n->relays[i]);
            nostr_relay_set_auth_callback(n->relays[i],
                                          md_nostr_auth_handler, &n->relay_states[i]);
        }
        update_acl_readiness(n);
        pthread_mutex_unlock(&n->allowlist_mu);
    }

    if (n->relay_count == 0) {
        /* A bridge with no usable relays cannot perform any operation */
        MD_LOG_E("no usable relays — bridge creation failed");
        md_nostr_destroy(n);
        return NULL;
    }

    if (n->cbs.on_dm) {
        if (pthread_create(&n->dm_thread, NULL, dm_worker, n) != 0) {
            md_nostr_destroy(n);
            return NULL;
        }
        n->dm_started = true;
    }

    for (size_t i = 0; i < n->relay_count; i++) {
        pthread_mutex_lock(&n->work_mu);
        n->auth_signers++;
        pthread_mutex_unlock(&n->work_mu);
        if (!nostr_relay_set_auth_signer_full(n->relays[i],
                NOSTR_RELAY_AUTH_ON_CHALLENGE,
                n->owner ? md_account_binding_generation(n->owner) : 1,
                n->pk_hex, md_nostr_auth_sign, n, md_nostr_auth_signer_release)) {
            md_nostr_destroy(n);
            return NULL;
        }
    }

    /* Start pool worker threads */
    nostr_simple_pool_start(n->pool);

    /* One subscription with the full merged filter set. nostrc frees the
     * pool's shared filter set on each subscribe call, while live
     * subscriptions borrow it — so a second independent subscribe would
     * leave the first subscription matching against freed memory. */
    if (md_nostr_subscribe(n) != 0) {
        pthread_mutex_lock(&n->allowlist_mu);
        n->acl_sub_failed = true; /* no REQ: no boundary will ever arrive */
        update_acl_readiness(n);
        pthread_mutex_unlock(&n->allowlist_mu);
    }

    /* Publish the revocation watch only after the immutable relay snapshot
     * and every AUTH registration exist. A role revoked during construction
     * calls back synchronously here and we unwind; stop can never race the
     * snapshot or be followed by installing another AUTH signer. */
    if (n->owner) {
        n->owner_watch = md_account_binding_watch(n->owner, owner_revoked, n);
        if (!n->owner_watch || is_stopped(n)) { md_nostr_destroy(n); return NULL; }
    }

    MD_LOG_I("bridge ready (signer=%s, pk=%.*s..., relays=%zu)", md_signer_type_name(md_signer_get_type(n->signer)), 8, n->pk_hex, n->relay_count);

    return n;
}

const char *md_nostr_get_npub(const MdNostr *n) {
    return n ? n->pk_hex : NULL;
}

MdSigner *md_nostr_get_signer(MdNostr *n) {
    return n ? n->signer : NULL;
}

void md_nostr_stop(MdNostr *n) {
    if (!n) return;
    pthread_mutex_lock(&n->work_mu);
    n->closing = true;
    md_signer_cancel(n->signer_cancel);
    pthread_cond_broadcast(&n->work_cv);
    pthread_mutex_unlock(&n->work_mu);
    for (size_t i = 0; i < n->relay_count; i++)
        nostr_relay_set_auth_signer_full(n->relays[i], NOSTR_RELAY_AUTH_ON_CHALLENGE,
                                        0, NULL, NULL, NULL, NULL);
    pthread_mutex_lock(&n->allowlist_mu);
    pthread_cond_broadcast(&n->allowlist_cv);
    pthread_mutex_unlock(&n->allowlist_mu);
}

void md_nostr_destroy(MdNostr *n) {
    if (!n) return;
    md_account_binding_unwatch(n->owner_watch);
    md_nostr_stop(n);

    /* Stop relay producers before draining consumers. Keep the pool,
     * allowlist and callbacks alive while accepted work still uses them. */
    if (n->pool) nostr_simple_pool_stop(n->pool);
    test_point(n, MD_NOSTR_TEST_DRAINING);

    if (n->dm_started) pthread_join(n->dm_thread, NULL);
    pthread_mutex_lock(&n->work_mu);
    while (n->active_ops || n->auth_signers) pthread_cond_wait(&n->work_cv, &n->work_mu);
    pthread_mutex_unlock(&n->work_mu);
    if (n->pool) nostr_simple_pool_free(n->pool);
    if (n->allowlist)
        nostr_nip51_list_free(n->allowlist);
    cJSON_Delete(n->acl_private_unknown);

    /* Free cached relay snapshot */
    if (n->relay_urls) {
        for (size_t i = 0; i < n->relay_count; i++)
            free(n->relay_urls[i]);
        free(n->relay_urls);
    }
    free(n->relays);
    free(n->relay_states);

    pthread_cond_destroy(&n->allowlist_cv);
    pthread_mutex_destroy(&n->allowlist_mu);

    pthread_cond_destroy(&n->work_cv);
    pthread_mutex_destroy(&n->work_mu);
    md_signer_destroy(n->signer);
    md_account_binding_release(n->owner);
    md_signer_cancel_free(n->signer_cancel);

    free(n->quality_cipher);
    free(n->pk_hex);
    free(n);
}

void md_nostr_shutdown(void) {
    /* Run in the module that owns nostrc's statically linked runtime. */
    nostr_connection_shutdown();
}

/* ── Key utilities ────────────────────────────────────────── */

int md_nostr_generate_keypair(char **sk_hex_out, char **pk_hex_out) {
    if (!sk_hex_out || !pk_hex_out)
        return -1;

    char *sk = nostr_key_generate_private();
    if (!sk) return -1;

    char *pk = nostr_key_get_public(sk);
    if (!pk) {
        memset(sk, 0, strlen(sk));
        free(sk);
        return -1;
    }

    *sk_hex_out = sk;
    *pk_hex_out = pk;
    return 0;
}

char *md_nostr_get_pubkey(const char *sk_hex) {
    if (!sk_hex) return NULL;
    return nostr_key_get_public(sk_hex);
}

/* ── Internal: sign and serialize an event via signer ─────────
 *
 * Builds an unsigned event JSON, passes it to the signer for signing,
 * and returns a NostrEvent from the signed JSON.
 *
 * This is the universal signing path that works with all backends.
 * For remote signers (NIP-46/55L/5F), the JSON round-trips through
 * the signer protocol. For direct-key, it stays in-process.
 */
static NostrEvent *sign_event_via_signer(MdSigner *signer, NostrEvent *ev) {
    return sign_event_cancellable(signer, ev, NULL);
}

static NostrEvent *sign_event_cancellable(MdSigner *signer, NostrEvent *ev,
                                          const MdSignerCancel *cancel) {
    if (!signer || !ev) return NULL;

    /* Serialize unsigned event to JSON */
    char *unsigned_json = nostr_event_serialize(ev);
    if (!unsigned_json) return NULL;

    /* Sign through the signer abstraction */
    char *signed_json = NULL;
    int ret = md_signer_sign_event_cancellable(signer, unsigned_json, &signed_json, cancel);
    free(unsigned_json);

    if (ret != MD_SIGNER_OK || !signed_json)
        return NULL;

    /* Parse signed JSON back to NostrEvent */
    NostrEvent *signed_ev = event_from_json(signed_json);
    free(signed_json);

    return signed_ev;
}

/* ── Session signaling (NIP-17 gift-wrapped DMs) ────────────── */

int md_nostr_sign_event_json(MdSigner *signer, NostrEvent *ev,
                             char **out_json) {
    if (!signer || !ev || !out_json)
        return -1;

    NostrEvent *signed_ev = sign_event_via_signer(signer, ev);
    if (!signed_ev)
        return -1;

    *out_json = nostr_event_serialize(signed_ev);
    nostr_event_free(signed_ev);
    return *out_json ? 0 : -1;
}

/*
 * NIP-17 three-layer encryption:
 *   1. Rumor (kind:14) — unsigned message with session JSON
 *   2. Seal (kind:13) — NIP-44 encrypted rumor, signed by sender
 *   3. Gift-wrap (kind:1059) — NIP-44 encrypted seal, signed by ephemeral key
 *
 * The sender's key (via signer) is used for:
 *   - NIP-44 encrypt the rumor content into the seal
 *   - Sign the seal (kind:13)
 *
 * An ephemeral key (generated locally) is used for:
 *   - NIP-44 encrypt the seal into the gift-wrap
 *   - Sign the gift-wrap (kind:1059)
 */

static int md_nostr_send_dm(MdNostr *n, const char *recipient_pubkey_hex,
                            const char *content) {
    if (!n || !n->signer || !recipient_pubkey_hex || !content)
        return -1;

    /* Step 1: Rumor (kind:14, unsigned, p-tagged) via nostrc's NIP-17
     * builder — the p tag is what our own #p subscription filter and
     * compliant relays match on. */
    NostrEvent *rumor = nostr_nip17_create_rumor(n->pk_hex,
                                                 recipient_pubkey_hex,
                                                 content, 0);
    if (!rumor) return -1;

    char rumor_id[65];
    if (nostr_event_compute_id(rumor, rumor_id) != NOSTR_EVENT_VALIDATION_OK) {
        nostr_event_free(rumor);
        return -1;
    }
    rumor->id = strdup(rumor_id);
    if (!rumor->id) { nostr_event_free(rumor); return -1; }
    char *rumor_json = nostr_event_serialize(rumor);
    nostr_event_free(rumor);
    if (!rumor_json) return -1;

    /* Step 2: NIP-44 encrypt rumor → seal content (via signer) */
    char *encrypted_rumor = NULL;
    int ret = md_signer_nip44_encrypt_cancellable(n->signer, recipient_pubkey_hex,
                                      rumor_json, &encrypted_rumor, n->signer_cancel);
    free(rumor_json);
    if (ret != MD_SIGNER_OK || !encrypted_rumor)
        return -1;
    if (is_stopped(n)) { free(encrypted_rumor); return -1; }

    /* Step 3: Seal (kind:13) signed via the signer abstraction. This step
     * stays local: nostrc's create_seal needs the raw secret key, which
     * remote signers (NIP-46/55L/5F) do not expose. */
    NostrEvent *seal = nostr_event_new();
    if (!seal) { free(encrypted_rumor); return -1; }

    nostr_event_set_kind(seal, NOSTR_KIND_SEAL);
    nostr_event_set_content(seal, encrypted_rumor);
    nostr_event_set_pubkey(seal, n->pk_hex);
    nostr_event_set_created_at(seal, (int64_t)time(NULL));
    free(encrypted_rumor);

    NostrEvent *signed_seal = sign_event_cancellable(n->signer, seal, n->signer_cancel);
    nostr_event_free(seal);
    if (!signed_seal) return -1;
    if (is_stopped(n)) { nostr_event_free(signed_seal); return -1; }

    /* Step 4: Gift-wrap (kind:1059) via nostrc — adds the recipient p tag
     * and a randomized timestamp per NIP-59, signed by an ephemeral key
     * it generates internally. */
    NostrEvent *gift_wrap = nostr_nip17_create_gift_wrap(signed_seal,
                                                         recipient_pubkey_hex);
    nostr_event_free(signed_seal);
    if (!gift_wrap) return -1;

    /* Step 5: Publish gift-wrap to all connected relays */
    ret = publish_all(n, gift_wrap);
    nostr_event_free(gift_wrap);

    return ret < 0 ? -1 : 0;
}

int md_nostr_send_session_request(MdNostr *n, const char *host_pubkey_hex,
                                  const char *json_payload) {
    if (!n || (n->owner && md_account_binding_role(n->owner) != MD_ACCOUNT_CLIENT) ||
        !operation_begin(n)) return -1;
    int ret = md_nostr_send_dm(n, host_pubkey_hex, json_payload);
    if (is_stopped(n)) ret = -1;
    operation_end(n);
    return ret;
}

int md_nostr_send_session_accept(MdNostr *n, const char *client_pubkey_hex,
                                 const char *json_payload) {
    if (!owns_host(n) || !operation_begin(n)) return -1;
    int ret = md_nostr_send_dm(n, client_pubkey_hex, json_payload);
    if (is_stopped(n)) ret = -1;
    operation_end(n);
    return ret;
}

/* ── Access control (NIP-51 allowlists) ─────────────────────── */

/* Exact canonical spellings make unknown/duplicate/out-of-order tokens fail
 * closed; the wire version is deliberately outside the CLI vocabulary. */
static const char *const cap_names[] = {
    "none", "video", "agent", "video,agent", "input", "video,input",
    "agent,input", "video,agent,input"
};

int md_nostr_parse_caps(const char *text, uint32_t *out) {
    if (!out) return -1;
    *out = 0;
    if (text) for (uint32_t i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++) {
        if (!strcmp(text, cap_names[i])) { *out = i; return 0; }
    }
    return -1;
}

/* Caller holds allowlist_mu. Membership and scope are read atomically from
 * the same winning event. Never infer policy from a p tag's extra field. */
static uint32_t entry_grant(const NostrList *list, const char *pk) {
    if (!pk || strlen(pk) != 64 || strspn(pk, "0123456789abcdef") != 64) return 0;
    unsigned members = 0, grants = 0;
    uint32_t caps = 0;
    if (list && pk) for (size_t i = 0; i < list->count; i++) {
        NostrListEntry *e = list->entries[i];
        if (!e || !e->tag_name || !e->value || strcmp(e->value, pk)) continue;
        if (!strcmp(e->tag_name, "p")) members++;
        if (!strcmp(e->tag_name, "metadesk-caps")) {
            grants++;
            if (!e->extra || strncmp(e->extra, "v1:", 3) ||
                md_nostr_parse_caps(e->extra + 3, &caps)) return 0;
        }
    }
    return members == 1 && grants == 1 ? caps : 0;
}

uint32_t md_nostr_granted_capabilities_known(MdNostr *n, const char *pk,
                                            uint32_t requested, bool *known) {
    bool settled = true;
    uint32_t grant = 0;
    if (pk && strlen(pk) == 64 && strspn(pk, "0123456789abcdef") == 64 &&
        owns_host(n) && operation_begin(n)) {
        pthread_mutex_lock(&n->allowlist_mu);
        /* FAILED is definite (every relay refused): deny, not "unknown". */
        settled = n->acl_state != MD_NOSTR_ACL_UNKNOWN;
        grant = n->acl_state == MD_NOSTR_ACL_OPEN
            ? MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT
            : n->acl_state == MD_NOSTR_ACL_LOADED && !n->acl_review_required &&
              !n->acl_decode_failed ? entry_grant(n->allowlist, pk) : 0;
        pthread_mutex_unlock(&n->allowlist_mu);
        if (is_stopped(n)) { grant = 0; settled = true; }
        operation_end(n);
    }
    if (known) *known = settled;
    return requested & grant;
}

uint32_t md_nostr_granted_capabilities(MdNostr *n, const char *pk, uint32_t requested) {
    return md_nostr_granted_capabilities_known(n, pk, requested, NULL);
}


static bool is_allowed(MdNostr *n, const char *pubkey_hex) {
    if (!n || !pubkey_hex)
        return false;

    pthread_mutex_lock(&n->allowlist_mu);
    bool allowed = n->acl_state == MD_NOSTR_ACL_OPEN;
    if (n->acl_state == MD_NOSTR_ACL_LOADED && !n->acl_review_required &&
        !n->acl_decode_failed && n->allowlist) {
        for (size_t i = 0; i < n->allowlist->count; i++) {
            NostrListEntry *entry = n->allowlist->entries[i];
            if (entry && entry->tag_name && strcmp(entry->tag_name, "p") == 0
                && entry->value && strcmp(entry->value, pubkey_hex) == 0) {
                allowed = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&n->allowlist_mu);
    return allowed;
}

static bool has_allowlist(const MdNostr *n) {
    if (!n)
        return false;

    pthread_mutex_lock((pthread_mutex_t *)&n->allowlist_mu);
    bool has = n->acl_state == MD_NOSTR_ACL_LOADED;
    pthread_mutex_unlock((pthread_mutex_t *)&n->allowlist_mu);
    return has;
}

static int refresh_allowlist(MdNostr *n) {
    if (!n || !n->pool || !n->pk_hex) return -1;

    /* Already subscribed at create; do not replace borrowed pool filters.
     * EOSE/CLOSED callbacks, not a refresh call or timer, drive readiness. */
    return 0;
}

/* No signer calls under allowlist_mu: external signers may block. The edit
 * reservation and version fence cover encryption, signing and publish together. */
static NostrEvent *build_allowlist_event(MdNostr *n, const NostrList *list,
                                        int64_t created, cJSON *items) {
    bool ok = items != NULL;
    if (list) for (size_t i = 0; ok && i < list->count; i++) {
        NostrListEntry *e = list->entries[i];
        if (!e || !e->tag_name || !e->value) continue;
        const char *fields[] = {e->tag_name, e->value, e->extra};
        cJSON *item = cJSON_CreateStringArray(fields, e->extra ? 3 : 2);
        if (!item || !cJSON_AddItemToArray(items, item)) {
            cJSON_Delete(item); ok = false;
        }
    }
    char *plain = ok ? cJSON_PrintUnformatted(items) : NULL;
    char *cipher = NULL;
    ok = plain && strlen(plain) <= MD_ALLOWLIST_MAX_CONTENT &&
        md_signer_nip44_encrypt_cancellable(n->signer, n->pk_hex, plain,
                                           &cipher, n->signer_cancel) == 0 && cipher &&
        strlen(cipher) <= MD_ALLOWLIST_MAX_CONTENT;
    if (plain) { md_secure_zero(plain, strlen(plain)); free(plain); }
    cJSON_Delete(items);
    NostrEvent *ev = ok ? nostr_event_new() : NULL;
    if (ev) {
        NostrTags *tags = nostr_tags_new(0);
        NostrTag *d = nostr_tag_new("d", "metadesk-allowlist", NULL);
        if (!tags || !d) {
            nostr_tags_free(tags); nostr_tag_free(d); nostr_event_free(ev); ev = NULL;
        } else {
            nostr_tags_append(tags, d);
            if (nostr_tags_size(tags) != 1) {
                nostr_tag_free(d); nostr_tags_free(tags); nostr_event_free(ev); ev = NULL;
            } else {
                nostr_event_set_tags(ev, tags);
                nostr_event_set_kind(ev, NOSTR_KIND_CATEGORIZED_PEOPLE_LIST);
                nostr_event_set_pubkey(ev, n->pk_hex);
                nostr_event_set_created_at(ev, created);
                nostr_event_set_content(ev, cipher);
            }
        }
    }
    free(cipher);
    return ev;
}

static int allowlist_edit(MdNostr *n, const char *pk, const char *caps, bool add,
                           const MdAllowlistEntry *confirmed, size_t count, bool migrate) {
    if (!n || !n->signer || (!migrate && (!pk || strlen(pk) != 64 ||
        strspn(pk, "0123456789abcdef") != 64))) return -1;

    uint32_t parsed = MD_CAP_VIDEO;
    if (add && caps && md_nostr_parse_caps(caps, &parsed)) return -1;
    char wire[64];
    snprintf(wire, sizeof(wire), "v1:%s", cap_names[parsed]);
    int ret = -1;
    NostrList *candidate = NULL;
    cJSON *private_items = NULL;
    NostrEvent *ev = NULL, *signed_ev = NULL;
    pthread_mutex_lock(&n->allowlist_mu);
    /* Existing callers may edit immediately after create. Wait for an actual
     * history/failure boundary, never build from a partially received list.
     * UNKNOWN with no relay pending (all down/awaiting AUTH) fails at once. */
    while (n->acl_state == MD_NOSTR_ACL_UNKNOWN && history_pending_locked(n) && !is_stopped(n))
        pthread_cond_wait(&n->allowlist_cv, &n->allowlist_mu);
    if (is_stopped(n) || n->acl_state != MD_NOSTR_ACL_LOADED || n->acl_editing ||
        n->acl_unconfirmed_count == MD_ACL_UNCONFIRMED_MAX || n->acl_decode_failed ||
        (!migrate && n->acl_review_required)) {
        pthread_mutex_unlock(&n->allowlist_mu);
        return -1;
    }
    /* Migration confirms the complete membership, not a partial union. Duplicate
     * source records normalize only through this explicit per-principal review. */
    if (migrate) {
        bool valid = count == 0 || confirmed;
        for (size_t i = 0; valid && i < count; i++) {
            const char *key = confirmed[i].pubkey_hex;
            uint32_t mask;
            valid = strlen(key) == 64 && strspn(key, "0123456789abcdef") == 64 &&
                !md_nostr_parse_caps(confirmed[i].caps, &mask);
            for (size_t j = 0; j < i; j++)
                if (!strcmp(key, confirmed[j].pubkey_hex)) valid = false;
            bool found = false;
            if (n->allowlist) for (size_t j = 0; j < n->allowlist->count; j++) {
                NostrListEntry *e = n->allowlist->entries[j];
                if (e && e->tag_name && e->value && !strcmp(e->tag_name, "p") &&
                    !strcmp(e->value, key)) found = true;
            }
            if (!found) valid = false;
        }
        if (n->allowlist) for (size_t i = 0; valid && i < n->allowlist->count; i++) {
            NostrListEntry *e = n->allowlist->entries[i];
            if (!e || !e->tag_name || !e->value || strcmp(e->tag_name, "p")) continue;
            bool found = false;
            for (size_t j = 0; j < count; j++)
                if (!strcmp(e->value, confirmed[j].pubkey_hex)) found = true;
            if (!found) valid = false;
        }
        if (!valid) { pthread_mutex_unlock(&n->allowlist_mu); return -1; }
    }
    unsigned members = 0, scopes = 0;
    bool same = false;
    if (!migrate && n->allowlist) for (size_t i = 0; i < n->allowlist->count; i++) {
        NostrListEntry *e = n->allowlist->entries[i];
        if (!e || !e->tag_name || !e->value || strcmp(e->value, pk)) continue;
        if (!strcmp(e->tag_name, "p")) members++;
        if (!strcmp(e->tag_name, "metadesk-caps")) {
            scopes++;
            same = e->extra && !strcmp(e->extra, wire);
        }
    }
    if (!migrate && ((!add && !members && !scopes) || (add && members &&
        (!caps || (members == 1 && scopes == 1 && same))))) {
        /* Omitted caps preserves existing policy; explicit caps is an upsert. */
        pthread_mutex_unlock(&n->allowlist_mu);
        return 0;
    }
    candidate = nostr_nip51_list_new();
    bool built = candidate != NULL;
    if (built) {
        nostr_nip51_list_set_identifier(candidate, "metadesk-allowlist");
        built = candidate->identifier != NULL;
    }
    if (built && !migrate && n->allowlist) for (size_t i = 0; i < n->allowlist->count; i++) {
        NostrListEntry *e = n->allowlist->entries[i];
        if (!e || !e->tag_name || !e->value) continue;
        if ((!strcmp(e->tag_name, "p") || !strcmp(e->tag_name, "metadesk-caps")) &&
            !strcmp(e->value, pk)) continue;
        if (!list_append_copy(candidate, e->tag_name, e->value, e->extra)) {
            built = false; break;
        }
    }
    if (migrate) for (size_t i = 0; built && i < count; i++) {
        snprintf(wire, sizeof(wire), "v1:%s", confirmed[i].caps);
        built = list_append_copy(candidate, "p", confirmed[i].pubkey_hex, NULL) &&
                list_append_copy(candidate, "metadesk-caps", confirmed[i].pubkey_hex, wire);
    }
    if (built && !migrate && add) built = list_append_copy(candidate, "p", pk, NULL) &&
                             list_append_copy(candidate, "metadesk-caps", pk, wire);
    uint64_t version = n->acl_version;
    int64_t created = (int64_t)time(NULL);
    /* Successive local edits within a second must actually replace the base,
     * independent of the random lexical order of their signed ids. */
    if (n->acl_id[0] && created <= n->acl_created_at) {
        if (n->acl_created_at == INT64_MAX) built = false;
        else created = n->acl_created_at + 1;
    }
    if (created > (int64_t)time(NULL) + 300) built = false;
    if (built) {
        private_items = n->acl_private_unknown ? cJSON_Duplicate(n->acl_private_unknown, true) : cJSON_CreateArray();
        built = private_items != NULL;
    }
    if (built) n->acl_editing = true;
    pthread_mutex_unlock(&n->allowlist_mu);
    if (built) {
        ev = build_allowlist_event(n, candidate, created, private_items); /* consumes items */
        private_items = NULL;
    }
    cJSON_Delete(private_items);
    if (!ev) {
        nostr_nip51_list_free(candidate);
        if (built) {
            pthread_mutex_lock(&n->allowlist_mu);
            n->acl_editing = false;
            pthread_mutex_unlock(&n->allowlist_mu);
        }
        return -1;
    }

    signed_ev = sign_event_cancellable(n->signer, ev, n->signer_cancel);
    nostr_event_free(ev);
    char id[65];
    if (signed_ev && nostr_event_compute_id(signed_ev, id) == NOSTR_EVENT_VALIDATION_OK) {
        pthread_mutex_lock(&n->allowlist_mu);
        bool current = version == n->acl_version && n->acl_state == MD_NOSTR_ACL_LOADED;
        size_t slot = n->acl_unconfirmed_count;
        if (current) {
            memcpy(n->acl_unconfirmed[slot], id, 65);
            n->acl_unconfirmed_count++;
        }
        pthread_mutex_unlock(&n->allowlist_mu);
        if (current) {
            test_point(n, MD_NOSTR_TEST_ALLOWLIST_SIGNED);
            ret = publish_all(n, signed_ev);
            pthread_mutex_lock(&n->allowlist_mu);
            if (ret == 0 && version == n->acl_version &&
                n->acl_state == MD_NOSTR_ACL_LOADED && !is_stopped(n)) {
                nostr_nip51_list_free(n->allowlist);
                n->allowlist = candidate;
                n->acl_review_required = n->acl_decode_failed = false;
                candidate = NULL;
                n->acl_created_at = nostr_event_get_created_at(signed_ev);
                memcpy(n->acl_id, id, 65);
                n->acl_version++;
                pthread_cond_broadcast(&n->allowlist_cv);
                /* Only this edit can append while acl_editing is true. */
                n->acl_unconfirmed_count--;
            } else ret = -1;
            pthread_mutex_unlock(&n->allowlist_mu);
        }
    }
    nostr_event_free(signed_ev);
    nostr_nip51_list_free(candidate);
    pthread_mutex_lock(&n->allowlist_mu);
    n->acl_editing = false;
    pthread_mutex_unlock(&n->allowlist_mu);
    return ret;
}

static int allowlist_add(MdNostr *n, const char *pk, const char *caps) {
    return allowlist_edit(n, pk, caps, true, NULL, 0, false);
}
static int allowlist_remove(MdNostr *n, const char *pk) {
    return allowlist_edit(n, pk, NULL, false, NULL, 0, false);
}

static int allowlist_count(const MdNostr *n) {
    if (!n) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&n->allowlist_mu);
    int count = 0;
    if (n->allowlist) for (size_t i = 0; i < n->allowlist->count; i++) {
        NostrListEntry *e = n->allowlist->entries[i];
        if (e && e->tag_name && !strcmp(e->tag_name, "p")) count++;
    }
    pthread_mutex_unlock((pthread_mutex_t *)&n->allowlist_mu);
    return count;
}

static int allowlist_get_entry(const MdNostr *n, int index,
                                 MdAllowlistEntry *out) {
    if (!n || !out || index < 0)
        return -1;

    pthread_mutex_lock((pthread_mutex_t *)&n->allowlist_mu);
    int ret = -1;
    if (n->allowlist) for (size_t i = 0; i < n->allowlist->count; i++) {
        NostrListEntry *entry = n->allowlist->entries[i];
        if (!entry || !entry->tag_name || strcmp(entry->tag_name, "p")) continue;
        if (index-- == 0) {
            /* Copy into caller storage — interior pointers into the list
             * would dangle as soon as a refresh replaces it. */
            snprintf(out->pubkey_hex, sizeof(out->pubkey_hex), "%s",
                     entry->value ? entry->value : "");
            snprintf(out->caps, sizeof(out->caps), "%s",
                     cap_names[n->acl_review_required ? 0 : entry_grant(n->allowlist, entry->value)]);
            out->review_required = n->acl_review_required;
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)&n->allowlist_mu);
    return ret;
}

int md_nostr_allowlist_migrate(MdNostr *n, const MdAllowlistEntry *confirmed, size_t count) {
    if (!owns_host(n) || !operation_begin(n)) return -1;
    int ret = allowlist_edit(n, NULL, NULL, false, confirmed, count, true);
    if (is_stopped(n)) ret = -1;
    operation_end(n);
    return ret;
}

int md_nostr_allowlist_snapshot(MdNostr *n, MdAllowlistEntry **entries, size_t *count) {
    if (!entries || !count) return -1;
    *entries = NULL;
    *count = 0;
    if (!owns_host(n) || !operation_begin(n)) return -1;
    int ret = -1;
    MdAllowlistEntry *copy = NULL;
    size_t used = 0;
    pthread_mutex_lock(&n->allowlist_mu);
    while (n->acl_state == MD_NOSTR_ACL_UNKNOWN && history_pending_locked(n) && !is_stopped(n))
        pthread_cond_wait(&n->allowlist_cv, &n->allowlist_mu);
    if (n->acl_state == MD_NOSTR_ACL_LOADED && !n->acl_decode_failed && !is_stopped(n)) {
        size_t capacity = n->allowlist ? n->allowlist->count : 0;
        copy = capacity ? calloc(capacity, sizeof(*copy)) : NULL;
        if (!capacity || copy) {
            for (size_t i = 0; i < capacity; ++i) {
                NostrListEntry *e = n->allowlist->entries[i];
                if (!e || !e->tag_name || strcmp(e->tag_name, "p") || !e->value) continue;
                snprintf(copy[used].pubkey_hex, sizeof(copy[used].pubkey_hex), "%s", e->value);
                snprintf(copy[used].caps, sizeof(copy[used].caps), "%s",
                         cap_names[n->acl_review_required ? 0 : entry_grant(n->allowlist, e->value)]);
                copy[used].review_required = n->acl_review_required;
                ++used;
            }
            ret = 0;
        }
    }
    pthread_mutex_unlock(&n->allowlist_mu);
    if (is_stopped(n)) ret = -1;
    operation_end(n);
    if (ret) free(copy);
    else { *entries = copy; *count = used; }
    return ret;
}

int md_nostr_allowlist_wait_change(MdNostr *n, MdNostrAclState *state,
                                   uint64_t *version) {
    if (!state || !version || !owns_host(n) || !operation_begin(n)) return -1;
    pthread_mutex_lock(&n->allowlist_mu);
    while (n->acl_state == *state && n->acl_version == *version && !is_stopped(n))
        pthread_cond_wait(&n->allowlist_cv, &n->allowlist_mu);
    *state = n->acl_state;
    *version = n->acl_version;
    pthread_mutex_unlock(&n->allowlist_mu);
    int ret = is_stopped(n) ? -1 : 0;
    operation_end(n);
    return ret;
}

size_t md_nostr_allowlist_refusals(MdNostr *n, char *buf, size_t len) {
    if (buf && len) buf[0] = '\0';
    if (!owns_host(n) || !operation_begin(n)) return 0;
    size_t count = 0, used = 0;
    pthread_mutex_lock(&n->allowlist_mu);
    for (size_t i = 0; i < n->relay_count; i++) {
        MdRelayState *rs = &n->relay_states[i];
        if (!relay_refused_locked(n, rs)) continue;
        if (buf && used < len) {
            int w = snprintf(buf + used, len - used, "%s%s: %s", count ? "; " : "",
                             n->relay_urls[i], rs->closed_reason);
            if (w > 0) used += (size_t)w;
        }
        count++;
    }
    pthread_mutex_unlock(&n->allowlist_mu);
    operation_end(n);
    return count;
}

MdNostrAclState md_nostr_allowlist_state(MdNostr *n) {
    if (!owns_host(n) || !operation_begin(n)) return MD_NOSTR_ACL_FAILED;
    pthread_mutex_lock(&n->allowlist_mu);
    MdNostrAclState state = n->acl_state;
    pthread_mutex_unlock(&n->allowlist_mu);
    if (is_stopped(n)) state = MD_NOSTR_ACL_FAILED;
    operation_end(n);
    return state;
}

bool md_nostr_is_allowed(MdNostr *n, const char *pk) {
    if (!owns_host(n) || !operation_begin(n)) return false;
    bool ret = is_allowed(n, pk);
    if (is_stopped(n)) ret = false;
    operation_end(n);
    return ret;
}
bool md_nostr_has_allowlist(const MdNostr *n) {
    if (!owns_host((MdNostr *)n) || !operation_begin((MdNostr *)n)) return false;
    bool ret = has_allowlist(n);
    if (is_stopped((MdNostr *)n)) ret = false;
    operation_end((MdNostr *)n);
    return ret;
}
int md_nostr_refresh_allowlist(MdNostr *n) {
    if (!owns_host(n) || !operation_begin(n)) return -1;
    int ret = refresh_allowlist(n);
    if (is_stopped(n)) ret = -1;
    operation_end(n);
    return ret;
}
int md_nostr_allowlist_count(const MdNostr *n) {
    if (!owns_host((MdNostr *)n) || !operation_begin((MdNostr *)n)) return 0;
    int ret = allowlist_count(n);
    if (is_stopped((MdNostr *)n)) ret = 0;
    operation_end((MdNostr *)n);
    return ret;
}
int md_nostr_allowlist_get_entry(const MdNostr *n, int index, MdAllowlistEntry *out) {
    if (!owns_host((MdNostr *)n) || !operation_begin((MdNostr *)n)) return -1;
    int ret = allowlist_get_entry(n, index, out);
    if (is_stopped((MdNostr *)n)) {
        if (out) memset(out, 0, sizeof(*out));
        ret = -1;
    }
    operation_end((MdNostr *)n);
    return ret;
}

int md_nostr_publish_signed_json(MdNostr *n, const char *json) {
    if (!operation_begin(n)) return -1;
    int ret = publish_signed_json(n, json);
    if (is_stopped(n)) ret = -1;
    operation_end(n);
    return ret;
}

/* ── Generic event publishing ────────────────────────────────── */

int md_nostr_allowlist_add(MdNostr *n, const char *pk, const char *caps) {
    if (!owns_host(n) || !operation_begin(n)) return -1;
    int ret = allowlist_add(n, pk, caps);
    if (is_stopped(n)) ret = -1;
    operation_end(n);
    return ret;
}

int md_nostr_allowlist_remove(MdNostr *n, const char *pk) {
    if (!owns_host(n) || !operation_begin(n)) return -1;
    int ret = allowlist_remove(n, pk);
    if (is_stopped(n)) ret = -1;
    operation_end(n);
    return ret;
}

static int publish_signed_json(MdNostr *n, const char *signed_event_json) {
    if (!n || !signed_event_json || n->relay_count == 0)
        return -1;

    /* Parse the JSON into a NostrEvent for publish_all */
    NostrEvent *ev = event_from_json(signed_event_json);
    if (!ev) {
        MD_LOG_I("publish_signed_json: failed to parse event");
        return -1;
    }

    if (n->owner) {
        const char *author = nostr_event_get_pubkey(ev);
        char canonical[65];
        if (!author || strcmp(author, n->pk_hex) ||
            nostr_event_validate(ev, canonical) != NOSTR_EVENT_VALIDATION_OK) {
            nostr_event_free(ev);
            return -1;
        }
    }
    int ret = publish_all(n, ev);
    nostr_event_free(ev);

    if (ret == 0) {
        MD_LOG_I("published signed event");
    }

    return ret;
}


static bool quality_host_key(const char *s) {
    return s && strlen(s)==64 && strspn(s,"0123456789abcdef")==64;
}
static cJSON *quality_document(MdNostr *n) {
    pthread_mutex_lock(&n->allowlist_mu);
    bool exists = n->quality_cipher != NULL;
    char *cipher = exists ? strdup(n->quality_cipher) : NULL;
    pthread_mutex_unlock(&n->allowlist_mu);
    if (!exists) {
        cJSON *doc = cJSON_CreateObject();
        if (doc) { cJSON_AddNumberToObject(doc,"v",1); cJSON_AddObjectToObject(doc,"hosts"); }
        return doc;
    }
    char *plain = NULL;
    int rc = cipher ? md_signer_nip44_decrypt_cancellable(n->signer,n->pk_hex,cipher,&plain,n->signer_cancel) : -1;
    free(cipher);
    cJSON *doc = !rc && plain && strlen(plain)<=32768 && allowlist_json_strings_safe(plain)
        ? cJSON_ParseWithOpts(plain,NULL,true) : NULL;
    if (plain) { md_secure_zero(plain,strlen(plain)); free(plain); }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(doc,"v"), *hosts = cJSON_GetObjectItemCaseSensitive(doc,"hosts");
    if (!cJSON_IsObject(doc) || !cJSON_IsNumber(v) || v->valuedouble!=1 || !cJSON_IsObject(hosts)) {
        cJSON_Delete(doc); return NULL;
    }
    return doc;
}
int md_nostr_quality_load(MdNostr *n,const char *host,MdQuality *out) {
    if (!n || !n->quality_preferences || !out || !quality_host_key(host) ||
        md_nostr_wait_history(n,5000) || !operation_begin(n)) return -1;
    cJSON *doc=quality_document(n); bool present=false;
    cJSON *entry=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(doc,"hosts"),host);
    int rc=doc ? (!entry ? 1 : md_quality_read(entry,out,&present) || !present ? -1 : 0) : -1;
    cJSON_Delete(doc); if (is_stopped(n)) rc=-1; operation_end(n); return rc;
}
int md_nostr_quality_save(MdNostr *n,const char *host,const MdQuality *q) {
    if (!n || !n->quality_preferences || !quality_host_key(host) || !md_quality_valid(q) ||
        md_nostr_wait_history(n,5000) || !operation_begin(n)) return -1;
    pthread_mutex_lock(&n->allowlist_mu);
    bool busy=n->quality_saving; n->quality_saving=true;
    int64_t created=n->quality_created; char base[65]; strcpy(base,n->quality_id);
    pthread_mutex_unlock(&n->allowlist_mu);
    if (busy) { operation_end(n); return -1; }
    int rc=-1; cJSON *doc=quality_document(n); char *plain=NULL,*cipher=NULL;
    NostrEvent *ev=NULL,*signed_ev=NULL;
    if (!doc) goto done;
    cJSON *hosts=cJSON_GetObjectItemCaseSensitive(doc,"hosts");
    cJSON *entry=cJSON_GetObjectItemCaseSensitive(hosts,host);
    if (!entry) entry=cJSON_AddObjectToObject(hosts,host);
    if (!cJSON_IsObject(entry)) goto done;
    cJSON_DeleteItemFromObjectCaseSensitive(entry,"quality");
    if (md_quality_write(entry,q)) goto done;
    plain=cJSON_PrintUnformatted(doc);
    if (!plain || strlen(plain)>32768 || md_signer_nip44_encrypt_cancellable(n->signer,n->pk_hex,
        plain,&cipher,n->signer_cancel) || !cipher) goto done;
    ev=nostr_event_new(); if (!ev) goto done;
    NostrTags *tags=nostr_tags_new(0); NostrTag *d=nostr_tag_new("d","metadesk-session-preferences",NULL);
    if (!tags || !d) { nostr_tags_free(tags); nostr_tag_free(d); goto done; }
    nostr_tags_append(tags,d); nostr_event_set_tags(ev,tags);
    nostr_event_set_kind(ev,30078); nostr_event_set_pubkey(ev,n->pk_hex);
    int64_t now=(int64_t)time(NULL); if (created>=now) now=created+1;
    if (now>(int64_t)time(NULL)+30) goto done;
    nostr_event_set_created_at(ev,now); nostr_event_set_content(ev,cipher);
    signed_ev=sign_event_cancellable(n->signer,ev,n->signer_cancel);
    pthread_mutex_lock(&n->allowlist_mu);
    bool current=!strcmp(base,n->quality_id);
    pthread_mutex_unlock(&n->allowlist_mu);
    if (signed_ev && current && !is_stopped(n)) rc=publish_all(n,signed_ev);
 done:
    if (plain) { md_secure_zero(plain,strlen(plain)); free(plain); }
    free(cipher); cJSON_Delete(doc); nostr_event_free(signed_ev); nostr_event_free(ev);
    pthread_mutex_lock(&n->allowlist_mu); n->quality_saving=false; pthread_mutex_unlock(&n->allowlist_mu);
    if (is_stopped(n)) rc=-1; operation_end(n); return rc;
}
