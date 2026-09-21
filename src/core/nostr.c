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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <json.h>
#include <go.h>
#include <nostr-kinds.h>
#include "log.h"
#define MD_LOG_TAG "nostr"

/* Forward declaration — defined after DM helpers, used by auth handler */
static NostrEvent *sign_event_via_signer(MdSigner *signer, NostrEvent *ev);

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

struct MdNostr {
    MdSigner          *signer;       /* signing backend                     */
    bool               owns_signer;  /* true if we created it (from sk_hex) */
    char              *pk_hex;       /* our public key, hex                 */
    NostrSimplePool   *pool;         /* nostrc relay pool — persistent conns */
    NostrList         *allowlist;    /* cached NIP-51 allowlist             */
    pthread_mutex_t    allowlist_mu; /* guards allowlist + refresh flags    */
    bool               allowlist_refresh_requested;
    bool               allowlist_refresh_loaded;
    MdNostrCallbacks   cbs;          /* event callbacks                     */

    /* Cached relay snapshot — avoids direct access to pool struct internals.
     * Populated once in md_nostr_create() after ensure_relay, before start.
     * Replace with nostrc accessor APIs when available. */
    NostrRelay       **relays;       /* borrowed relay pointers (not owned) */
    size_t             relay_count;
    char             **relay_urls;   /* owned copies of URL strings         */

    /* Tracks pending DM-unwrap goroutines so md_nostr_destroy() can
     * wait for them to finish before freeing signer/callbacks. */
    GoWaitGroup        dm_wg;
};

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
    if (!n || !ev || n->relay_count == 0) return -1;

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

/* ── NIP-42 AUTH callback ──────────────────────────────────────
 * Fires on the relay worker thread when a relay sends an AUTH
 * challenge.  We respond by building a kind:22242 event with the
 * relay URL and challenge string, signing it via the signer
 * abstraction, and publishing it back to the relay.
 *
 * This is required for relays that gate subscriptions or event
 * storage behind authentication.
 */
static void md_nostr_auth_handler(NostrRelay *relay, const char *challenge,
                                  void *user_data) {
    MdNostr *n = user_data;
    if (!n || !n->signer || !relay || !challenge)
        return;

    const char *relay_url = nostr_relay_get_url_const(relay);
    MD_LOG_I("NIP-42 AUTH challenge from %s", relay_url ? relay_url : "(unknown)");

    /* Build kind:22242 auth event with relay + challenge tags */
    NostrEvent *ev = nostr_event_new();
    if (!ev) return;

    nostr_event_set_kind(ev, NOSTR_KIND_CLIENT_AUTHENTICATION);
    nostr_event_set_content(ev, "");
    nostr_event_set_pubkey(ev, n->pk_hex);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));

    NostrTag *challenge_tag = nostr_tag_new("challenge", challenge, NULL);
    NostrTag *relay_tag = nostr_tag_new("relay", relay_url ? relay_url : "", NULL);
    if (!challenge_tag || !relay_tag) {
        if (challenge_tag) nostr_tag_free(challenge_tag);
        if (relay_tag) nostr_tag_free(relay_tag);
        nostr_event_free(ev);
        return;
    }
    NostrTags *tags = nostr_tags_new(2, relay_tag, challenge_tag);
    if (!tags) {
        nostr_tag_free(challenge_tag);
        nostr_tag_free(relay_tag);
        nostr_event_free(ev);
        return;
    }
    nostr_event_set_tags(ev, tags); /* takes ownership */

    /* Sign via signer abstraction (may block for remote signers) */
    NostrEvent *signed_ev = sign_event_via_signer(n->signer, ev);
    nostr_event_free(ev);
    if (!signed_ev) {
        MD_LOG_E("AUTH sign failed for %s", relay_url ? relay_url : "(unknown)");
        return;
    }

    /* Send the signed auth event back to the relay */
    nostr_relay_publish(relay, signed_ev);
    nostr_event_free(signed_ev);
    MD_LOG_I("AUTH response sent to %s", relay_url ? relay_url : "(unknown)");
}

/* ── Relay OK callback ─────────────────────────────────────────
 * Fires on the relay worker thread when a relay sends an OK
 * response (["OK","<event_id>",true/false,"<reason>"]) after we
 * publish an event.  We log rejections and forward to the
 * caller-provided callback if one is registered.
 */
static void md_nostr_ok_handler(const char *event_id, bool ok,
                                const char *reason, void *user_data) {
    MdNostr *n = user_data;
    if (!n) return;

    if (!ok) {
        MD_LOG_W("relay REJECTED event %.16s%s — %s", event_id ? event_id : "(null)", event_id && strlen(event_id) > 16 ? "..." : "", reason ? reason : "(no reason)");
    }

    if (n->cbs.on_publish_result) {
        n->cbs.on_publish_result(event_id, ok, reason,
                                 n->cbs.publish_result_userdata);
    }
}

/* ── DM unwrap goroutine ──────────────────────────────────────
 * Offloads the 4-step NIP-17 gift-wrap decrypt chain from the pool
 * worker thread.  Each decrypt may involve IPC to a remote signer,
 * so running them on dedicated OS threads prevents head-of-line
 * blocking for other incoming events.
 *
 * Lifecycle safety: the caller increments dm_wg before spawning;
 * md_nostr_destroy() waits on dm_wg before freeing signer/cbs. */
typedef struct {
    char        *ephemeral_pk; /* owned copy of gift-wrap sender pubkey */
    char        *gw_content;   /* owned copy of gift-wrap ciphertext   */
    MdSigner    *signer;       /* borrowed — safe while dm_wg pending  */
    void       (*on_dm)(const char *, const char *, void *);
    void        *dm_userdata;
    GoWaitGroup *dm_wg;        /* decrement when done                  */
} DmUnwrapArg;

static void *dm_unwrap_thread(void *arg) {
    DmUnwrapArg *ua = arg;

    /* Step 1: Decrypt gift-wrap → seal JSON */
    char *seal_json = NULL;
    int ret = md_signer_nip44_decrypt(ua->signer, ua->ephemeral_pk,
                                      ua->gw_content, &seal_json);
    if (ret != 0 || !seal_json) goto done;

    /* Step 2: Parse seal to get sender pubkey and encrypted content */
    NostrEvent *seal = event_from_json(seal_json);
    free(seal_json);
    if (!seal) goto done;

    const char *sender_pk = nostr_event_get_pubkey(seal);
    const char *seal_content = nostr_event_get_content(seal);
    if (!sender_pk || !seal_content) {
        nostr_event_free(seal);
        goto done;
    }

    /* Step 3: Decrypt seal content → rumor JSON.
     * sender_pk is borrowed from seal; keep an owned copy for the
     * post-seal callback path before freeing seal below. */
    char *sender_pk_copy = strdup(sender_pk);
    if (!sender_pk_copy) {
        nostr_event_free(seal);
        goto done;
    }

    char *rumor_json = NULL;
    ret = md_signer_nip44_decrypt(ua->signer, sender_pk_copy,
                                  seal_content, &rumor_json);
    nostr_event_free(seal);
    if (ret != 0 || !rumor_json) {
        free(sender_pk_copy);
        goto done;
    }

    /* Step 4: Parse rumor to get DM content */
    NostrEvent *rumor = event_from_json(rumor_json);
    free(rumor_json);
    if (!rumor) {
        free(sender_pk_copy);
        goto done;
    }

    const char *dm_content = nostr_event_get_content(rumor);
    if (dm_content) {
        char *content_copy = strdup(dm_content);
        nostr_event_free(rumor);

        if (content_copy)
            ua->on_dm(sender_pk_copy, content_copy, ua->dm_userdata);

        free(content_copy);
    } else {
        nostr_event_free(rumor);
    }
    free(sender_pk_copy);

done:
    go_wait_group_done(ua->dm_wg);
    free(ua->ephemeral_pk);
    free(ua->gw_content);
    free(ua);
    return NULL;
}

/* ── Event middleware ─────────────────────────────────────────
 * nostrc's NostrSimplePool invokes the event_middleware callback
 * for every incoming event across all subscriptions. We route
 * events to the appropriate metadesk callback based on kind.
 *
 * This runs on the pool's worker thread — callbacks must be
 * thread-safe or dispatch to the main thread.
 */
static void md_nostr_event_handler(NostrIncomingEvent *incoming,
                                   void *user_data) {
    if (!incoming || !incoming->event)
        return;

    MdNostr *n = user_data;
    if (!n)
        return;
    NostrEvent *ev = incoming->event;

    int kind = nostr_event_get_kind(ev);

    if (kind == NOSTR_KIND_GIFT_WRAP && n->cbs.on_dm && n->signer) {
        /* NIP-17 gift-wrap: offload the 4-step decrypt chain to a
         * goroutine so the pool worker thread isn't blocked by
         * potentially slow signer IPC (remote/daemon signers). */
        const char *ephemeral_pk = nostr_event_get_pubkey(ev);
        const char *gw_content = nostr_event_get_content(ev);
        if (!ephemeral_pk || !gw_content) return;

        DmUnwrapArg *ua = malloc(sizeof(DmUnwrapArg));
        if (!ua) return;
        ua->ephemeral_pk = strdup(ephemeral_pk);
        ua->gw_content   = strdup(gw_content);
        ua->signer       = n->signer;
        ua->on_dm        = n->cbs.on_dm;
        ua->dm_userdata  = n->cbs.dm_userdata;
        ua->dm_wg        = &n->dm_wg;

        if (!ua->ephemeral_pk || !ua->gw_content) {
            free(ua->ephemeral_pk);
            free(ua->gw_content);
            free(ua);
            return;
        }

        go_wait_group_add(&n->dm_wg, 1);
        go(dm_unwrap_thread, ua);
    } else if (kind == NOSTR_KIND_CATEGORIZED_PEOPLE_LIST) {
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

        NostrList *list = nostr_nip51_parse_list(ev, NULL);
        if (list && list->identifier &&
            strcmp(list->identifier, "metadesk-allowlist") == 0) {
            /* Replace cached allowlist (pool worker thread vs callers) */
            pthread_mutex_lock(&n->allowlist_mu);
            if (n->allowlist)
                nostr_nip51_list_free(n->allowlist);
            n->allowlist = list;
            n->allowlist_refresh_loaded = true;
            size_t count = list->count;
            pthread_mutex_unlock(&n->allowlist_mu);
            MD_LOG_I("refreshed allowlist (%zu entries)", count);
        } else {
            nostr_nip51_list_free(list);
        }
    }
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
     * since = now - 300s: catch DMs from the last 5 minutes without
     * replaying ancient gift-wraps. */
    if (n->cbs.on_dm) {
        NostrFilter *f = nostr_filter_builder_build(
            nostr_filter_builder_since(
                nostr_filter_builder_tag(
                    nostr_filter_builder_kinds(
                        nostr_filter_builder_new(),
                        NOSTR_KIND_GIFT_WRAP, -1),
                    "p", n->pk_hex),
                (int64_t)time(NULL) - 300));
        if (f) {
            nostr_filters_add(filters, f);
            nostr_filter_free(f);
            added++;
        }
    }

    /* kind:30000, authors:[our_pk], #d:["metadesk-allowlist"].
     * limit:1 — addressable event (NIP-33), only the latest exists;
     * the subscription stays open for live replacements. */
    {
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

    if (added > 0)
        nostr_simple_pool_subscribe(n->pool, (const char **)n->relay_urls,
                                    n->relay_count, *filters, true);
    nostr_filters_free(filters);
    return added > 0 ? 0 : -1;
}

MdNostr *md_nostr_create(const MdNostrConfig *cfg, const MdNostrCallbacks *cbs) {
    if (!cfg || !cfg->relay_urls || cfg->relay_count <= 0)
        return NULL;

    /* Must have either signer or sk_hex */
    if (!cfg->signer && !cfg->sk_hex)
        return NULL;

    MdNostr *n = calloc(1, sizeof(MdNostr));
    if (!n) return NULL;

    pthread_mutex_init(&n->allowlist_mu, NULL);
    go_wait_group_init(&n->dm_wg);

    /* Set up signer: use provided signer, or create direct-key from sk_hex */
    if (cfg->signer) {
        n->signer = cfg->signer;
        n->owns_signer = false;
    } else {
        n->signer = md_signer_create_direct(cfg->sk_hex);
        if (!n->signer) {
            MD_LOG_E("failed to create signer from sk_hex");
            free(n);
            return NULL;
        }
        n->owns_signer = true;
    }

    /* Get pubkey from signer */
    char *pk = NULL;
    if (md_signer_get_pubkey(n->signer, &pk) != MD_SIGNER_OK || !pk) {
        MD_LOG_E("failed to get pubkey from signer");
        if (n->owns_signer) md_signer_destroy(n->signer);
        free(n);
        return NULL;
    }
    n->pk_hex = pk;

    /* Store callbacks */
    if (cbs)
        n->cbs = *cbs;

    /* Create relay pool — persistent WebSocket connections */
    n->pool = nostr_simple_pool_new();
    if (!n->pool) {
        if (n->owns_signer) md_signer_destroy(n->signer);
        free(n->pk_hex);
        free(n);
        return NULL;
    }

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

        /* Register per-relay callbacks.  Both have a user_data parameter
         * (unlike event_middleware), so we pass MdNostr directly. */
        for (size_t i = 0; i < n->relay_count; i++) {
            nostr_relay_set_ok_callback(n->relays[i],
                                        md_nostr_ok_handler, n);
            nostr_relay_set_auth_callback(n->relays[i],
                                          md_nostr_auth_handler, n);
        }
    }

    if (n->relay_count == 0) {
        /* A bridge with no usable relays cannot perform any operation */
        MD_LOG_E("no usable relays — bridge creation failed");
        md_nostr_destroy(n);
        return NULL;
    }

    /* Start pool worker threads */
    nostr_simple_pool_start(n->pool);

    /* One subscription with the full merged filter set. nostrc frees the
     * pool's shared filter set on each subscribe call, while live
     * subscriptions borrow it — so a second independent subscribe would
     * leave the first subscription matching against freed memory. */
    md_nostr_subscribe(n);

    MD_LOG_I("bridge ready (signer=%s, pk=%.*s..., relays=%zu)", md_signer_type_name(md_signer_get_type(n->signer)), 8, n->pk_hex, n->relay_count);

    return n;
}

const char *md_nostr_get_npub(const MdNostr *n) {
    return n ? n->pk_hex : NULL;
}

MdSigner *md_nostr_get_signer(MdNostr *n) {
    return n ? n->signer : NULL;
}

void md_nostr_destroy(MdNostr *n) {
    if (!n) return;

    /* Stop pool — closes all subscriptions, disconnects relays */
    if (n->pool) {
        nostr_simple_pool_stop(n->pool);
        nostr_simple_pool_free(n->pool);
    }
    if (n->allowlist)
        nostr_nip51_list_free(n->allowlist);

    /* Free cached relay snapshot */
    if (n->relay_urls) {
        for (size_t i = 0; i < n->relay_count; i++)
            free(n->relay_urls[i]);
        free(n->relay_urls);
    }
    free(n->relays);

    pthread_mutex_destroy(&n->allowlist_mu);

    /* Wait for any pending DM-unwrap goroutines before freeing signer */
    go_wait_group_wait(&n->dm_wg);
    go_wait_group_destroy(&n->dm_wg);

    /* Only destroy signer if we created it (from sk_hex fallback) */
    if (n->owns_signer && n->signer)
        md_signer_destroy(n->signer);

    free(n->pk_hex);
    free(n);
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
    if (!signer || !ev) return NULL;

    /* Serialize unsigned event to JSON */
    char *unsigned_json = nostr_event_serialize(ev);
    if (!unsigned_json) return NULL;

    /* Sign through the signer abstraction */
    char *signed_json = NULL;
    int ret = md_signer_sign_event(signer, unsigned_json, &signed_json);
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

    char *rumor_json = nostr_event_serialize(rumor);
    nostr_event_free(rumor);
    if (!rumor_json) return -1;

    /* Step 2: NIP-44 encrypt rumor → seal content (via signer) */
    char *encrypted_rumor = NULL;
    int ret = md_signer_nip44_encrypt(n->signer, recipient_pubkey_hex,
                                      rumor_json, &encrypted_rumor);
    free(rumor_json);
    if (ret != MD_SIGNER_OK || !encrypted_rumor)
        return -1;

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

    NostrEvent *signed_seal = sign_event_via_signer(n->signer, seal);
    nostr_event_free(seal);
    if (!signed_seal) return -1;

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
    return md_nostr_send_dm(n, host_pubkey_hex, json_payload);
}

int md_nostr_send_session_accept(MdNostr *n, const char *client_pubkey_hex,
                                 const char *json_payload) {
    return md_nostr_send_dm(n, client_pubkey_hex, json_payload);
}

/* ── Access control (NIP-51 allowlists) ─────────────────────── */

static void warn_allowlist_startup_window(const MdNostr *n) {
    if (!n || n->allowlist_refresh_loaded || n->allowlist)
        return;

    MD_LOG_W("WARNING: allowlist checked before first NIP-51 refresh (%s); startup window remains open until relay data arrives", n->allowlist_refresh_requested ? "waiting for relay data" : "refresh not requested");
}

bool md_nostr_is_allowed(MdNostr *n, const char *pubkey_hex) {
    if (!n || !pubkey_hex)
        return false;

    warn_allowlist_startup_window(n);

    pthread_mutex_lock(&n->allowlist_mu);
    bool allowed = false;
    if (n->allowlist) {
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

bool md_nostr_has_allowlist(const MdNostr *n) {
    if (!n)
        return false;

    warn_allowlist_startup_window(n);

    pthread_mutex_lock((pthread_mutex_t *)&n->allowlist_mu);
    bool has = n->allowlist && n->allowlist->count > 0;
    pthread_mutex_unlock((pthread_mutex_t *)&n->allowlist_mu);
    return has;
}

int md_nostr_refresh_allowlist(MdNostr *n) {
    if (!n || !n->pool || !n->pk_hex) return -1;

    /* The kind:30000 filter is part of the single merged subscription
     * created in md_nostr_create() — a second subscribe call would free
     * the filter set the live DM subscription borrows. Nothing to do
     * here but record the request; events arrive asynchronously. */
    n->allowlist_refresh_requested = true;
    return 0;
}

/* Build the signed-ready (unsigned) kind:30000 allowlist event from the
 * current cached list. Caller holds allowlist_mu. */
static NostrEvent *build_allowlist_event(const MdNostr *n) {
    NostrEvent *list_event = nostr_event_new();
    if (!list_event) return NULL;

    nostr_event_set_kind(list_event, NOSTR_KIND_CATEGORIZED_PEOPLE_LIST);
    nostr_event_set_pubkey(list_event, n->pk_hex);
    nostr_event_set_created_at(list_event, (int64_t)time(NULL));

    /* Serialize allowlist entries as NIP-51 tags:
     *   ["d", "metadesk-allowlist"]  — addressable d-tag
     *   ["p", "<pubkey>", "<caps>"]  — one per allowlist entry */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) { nostr_event_free(list_event); return NULL; }

    NostrTag *d_tag = nostr_tag_new("d", "metadesk-allowlist", NULL);
    if (d_tag) nostr_tags_append(tags, d_tag);

    if (n->allowlist) {
        for (size_t i = 0; i < n->allowlist->count; i++) {
            NostrListEntry *e = n->allowlist->entries[i];
            if (!e || !e->tag_name || !e->value) continue;
            NostrTag *t = e->extra
                ? nostr_tag_new(e->tag_name, e->value, e->extra, NULL)
                : nostr_tag_new(e->tag_name, e->value, NULL);
            if (t) nostr_tags_append(tags, t);
        }
    }

    nostr_event_set_tags(list_event, tags); /* takes ownership */
    return list_event;
}

int md_nostr_allowlist_add(MdNostr *n, const char *pubkey_hex, const char *caps) {
    if (!n || !n->signer || !pubkey_hex)
        return -1;

    NostrEvent *list_event = NULL;
    size_t count = 0;

    pthread_mutex_lock(&n->allowlist_mu);
    if (!n->allowlist)
        n->allowlist = nostr_nip51_list_new();
    if (n->allowlist) {
        n->allowlist_refresh_loaded = true;

        NostrListEntry *entry = nostr_nip51_entry_new("p", pubkey_hex, caps, false);
        if (entry) {
            nostr_nip51_list_add_entry(n->allowlist, entry);
            nostr_nip51_list_set_identifier(n->allowlist, "metadesk-allowlist");
            list_event = build_allowlist_event(n);
            count = n->allowlist->count;
        }
    }
    pthread_mutex_unlock(&n->allowlist_mu);

    if (!list_event) return -1;

    /* Sign via signer abstraction (may block — outside the mutex) */
    NostrEvent *signed_event = sign_event_via_signer(n->signer, list_event);
    nostr_event_free(list_event);
    if (!signed_event) return -1;

    int ret = publish_all(n, signed_event);
    nostr_event_free(signed_event);

    if (ret == 0)
        MD_LOG_I("published allowlist (%zu entries)", count);

    return ret;
}

int md_nostr_allowlist_remove(MdNostr *n, const char *pubkey_hex) {
    if (!n || !pubkey_hex)
        return -1;

    NostrEvent *list_event = NULL;
    size_t count = 0;

    pthread_mutex_lock(&n->allowlist_mu);
    if (n->allowlist) {
        /* Rebuild the list through the public API, dropping the matching
         * entry — nip51 exposes no remove, and mutating its entry array
         * in place depends on struct layout nostrc does not promise. */
        NostrList *kept = nostr_nip51_list_new();
        bool found = false;
        if (kept) {
            for (size_t i = 0; i < n->allowlist->count; i++) {
                NostrListEntry *e = n->allowlist->entries[i];
                if (!e || !e->tag_name || !e->value) continue;
                if (!found && strcmp(e->tag_name, "p") == 0 &&
                    strcmp(e->value, pubkey_hex) == 0) {
                    found = true;
                    continue;
                }
                NostrListEntry *copy =
                    nostr_nip51_entry_new(e->tag_name, e->value, e->extra, false);
                if (copy) nostr_nip51_list_add_entry(kept, copy);
            }
        }
        if (found && kept) {
            nostr_nip51_list_set_identifier(kept, "metadesk-allowlist");
            nostr_nip51_list_free(n->allowlist);
            n->allowlist = kept;
            kept = NULL;
            list_event = build_allowlist_event(n);
            count = n->allowlist->count;
        }
        if (kept) nostr_nip51_list_free(kept);
    }
    pthread_mutex_unlock(&n->allowlist_mu);

    if (!list_event) return -1;

    NostrEvent *signed_event = sign_event_via_signer(n->signer, list_event);
    nostr_event_free(list_event);
    if (!signed_event) return -1;

    int ret = publish_all(n, signed_event);
    nostr_event_free(signed_event);

    if (ret == 0)
        MD_LOG_I("published updated allowlist (%zu entries)", count);

    return ret;
}

int md_nostr_allowlist_count(const MdNostr *n) {
    if (!n) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&n->allowlist_mu);
    int count = n->allowlist ? (int)n->allowlist->count : 0;
    pthread_mutex_unlock((pthread_mutex_t *)&n->allowlist_mu);
    return count;
}

int md_nostr_allowlist_get_entry(const MdNostr *n, int index,
                                 MdAllowlistEntry *out) {
    if (!n || !out || index < 0)
        return -1;

    pthread_mutex_lock((pthread_mutex_t *)&n->allowlist_mu);
    int ret = -1;
    if (n->allowlist && (size_t)index < n->allowlist->count) {
        NostrListEntry *entry = n->allowlist->entries[index];
        if (entry) {
            /* Copy into caller storage — interior pointers into the list
             * would dangle as soon as a refresh replaces it. */
            snprintf(out->pubkey_hex, sizeof(out->pubkey_hex), "%s",
                     entry->value ? entry->value : "");
            snprintf(out->caps, sizeof(out->caps), "%s",
                     entry->extra ? entry->extra : "");
            ret = 0;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)&n->allowlist_mu);
    return ret;
}


/* ── Generic event publishing ────────────────────────────────── */

int md_nostr_publish_signed_json(MdNostr *n, const char *signed_event_json) {
    if (!n || !signed_event_json || n->relay_count == 0)
        return -1;

    /* Parse the JSON into a NostrEvent for publish_all */
    NostrEvent *ev = event_from_json(signed_event_json);
    if (!ev) {
        MD_LOG_I("publish_signed_json: failed to parse event");
        return -1;
    }

    int ret = publish_all(n, ev);
    nostr_event_free(ev);

    if (ret == 0) {
        MD_LOG_I("published signed event");
    }

    return ret;
}

