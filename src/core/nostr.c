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
#include <nip11.h>
#include <go.h>

/* ── Hex ↔ bytes helpers (nostr keys are hex, NIP-44 wants uint8_t[32]) ── */
static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    if (!hex) return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_char_val(hex[2 * i]);
        int lo = hex_char_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

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

/* Application-layer event dedup ring.  nostrc's pool-level dedup (unique=true)
 * catches most duplicates, but reconnect replays and cross-relay overlap can
 * still deliver the same event twice.  This is a small fixed-size ring that
 * tracks recent event IDs to guarantee idempotent processing. */
#define MD_DEDUP_RING_CAP 64

struct MdNostr {
    MdSigner          *signer;       /* signing backend                     */
    bool               owns_signer;  /* true if we created it (from sk_hex) */
    char              *pk_hex;       /* our public key, hex                 */
    NostrSimplePool   *pool;         /* nostrc relay pool — persistent conns */
    NostrList         *allowlist;    /* cached NIP-51 allowlist             */
    MdNostrCallbacks   cbs;          /* event callbacks                     */

    /* Cached relay snapshot — avoids direct access to pool struct internals.
     * Populated once in md_nostr_create() after ensure_relay, before start.
     * Replace with nostrc accessor APIs when available. */
    NostrRelay       **relays;       /* borrowed relay pointers (not owned) */
    size_t             relay_count;
    char             **relay_urls;   /* owned copies of URL strings         */

    /* Application-layer dedup ring (defense-in-depth over pool dedup) */
    char              *dedup_ids[MD_DEDUP_RING_CAP]; /* ring of event IDs   */
    size_t             dedup_head;                    /* next write slot     */
    size_t             dedup_len;                     /* entries used        */
    pthread_mutex_t    dedup_mu;                      /* guards ring access  */

    /* Tracks pending DM-unwrap goroutines so md_nostr_destroy() can
     * wait for them to finish before freeing signer/callbacks. */
    GoWaitGroup        dm_wg;
};

/* ── Dedup ring helpers ─────────────────────────────────────────
 * Returns true if the event ID was already seen (duplicate).
 * On first sight, records the ID and returns false.
 * Thread-safe: called from pool worker threads.
 */
static bool dedup_is_seen(MdNostr *n, const char *event_id) {
    if (!n || !event_id) return false;

    pthread_mutex_lock(&n->dedup_mu);

    /* Linear scan — ring is small (64 entries), O(n) is fine */
    size_t cap = n->dedup_len < MD_DEDUP_RING_CAP ? n->dedup_len : MD_DEDUP_RING_CAP;
    for (size_t i = 0; i < cap; i++) {
        if (n->dedup_ids[i] && strcmp(n->dedup_ids[i], event_id) == 0) {
            pthread_mutex_unlock(&n->dedup_mu);
            return true; /* duplicate */
        }
    }

    /* Not seen — insert at head, overwriting oldest if full */
    free(n->dedup_ids[n->dedup_head]); /* NULL-safe */
    n->dedup_ids[n->dedup_head] = strdup(event_id);
    n->dedup_head = (n->dedup_head + 1) % MD_DEDUP_RING_CAP;
    if (n->dedup_len < MD_DEDUP_RING_CAP)
        n->dedup_len++;

    pthread_mutex_unlock(&n->dedup_mu);
    return false;
}

/* ── Parallel relay publishing ──────────────────────────────── */
typedef struct {
    NostrRelay  *relay;
    NostrEvent  *event;
    GoWaitGroup *wg;
} PublishArg;

static void *publish_one_thread(void *arg) {
    PublishArg *pa = arg;
    nostr_relay_publish(pa->relay, pa->event);
    go_wait_group_done(pa->wg);
    free(pa);
    return NULL;
}

/* Helper: publish an event to all cached relays in parallel.
 * Each relay publish can block up to 5 s for enqueue confirmation,
 * so we fan out with go() + GoWaitGroup to overlap the waits. */
static int publish_all(MdNostr *n, NostrEvent *ev) {
    if (!n || !ev || n->relay_count == 0) return -1;

    GoWaitGroup wg;
    go_wait_group_init(&wg);

    for (size_t i = 0; i < n->relay_count; i++) {
        go_wait_group_add(&wg, 1);
        PublishArg *pa = malloc(sizeof(PublishArg));
        if (pa) {
            pa->relay = n->relays[i];
            pa->event = ev;
            pa->wg    = &wg;
            go(publish_one_thread, pa);
        } else {
            go_wait_group_done(&wg);
        }
    }

    go_wait_group_wait(&wg);
    go_wait_group_destroy(&wg);
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
    fprintf(stderr, "nostr: NIP-42 AUTH challenge from %s\n",
            relay_url ? relay_url : "(unknown)");

    /* Build kind:22242 auth event with relay + challenge tags */
    NostrEvent *ev = nostr_event_new();
    if (!ev) return;

    nostr_event_set_kind(ev, 22242);
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
        fprintf(stderr, "nostr: AUTH sign failed for %s\n",
                relay_url ? relay_url : "(unknown)");
        return;
    }

    /* Send the signed auth event back to the relay */
    nostr_relay_publish(relay, signed_ev);
    nostr_event_free(signed_ev);
    fprintf(stderr, "nostr: AUTH response sent to %s\n",
            relay_url ? relay_url : "(unknown)");
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
        fprintf(stderr, "nostr: relay REJECTED event %.16s%s — %s\n",
                event_id ? event_id : "(null)",
                event_id && strlen(event_id) > 16 ? "..." : "",
                reason ? reason : "(no reason)");
    }

    if (n->cbs.on_publish_result) {
        n->cbs.on_publish_result(event_id, ok, reason,
                                 n->cbs.publish_result_userdata);
    }
}

/* ── Instance registry ─────────────────────────────────────────
 * Maps NostrSimplePool* → MdNostr* so the event middleware can
 * route events to the correct instance without a global singleton.
 *
 * nostrc's event_middleware callback signature has no user_data
 * parameter, so we identify the owning MdNostr by matching the
 * incoming relay pointer against each registered pool's relay list.
 *
 * The relay array is populated in md_nostr_create() before
 * nostr_simple_pool_start(), and is not modified during runtime,
 * so reading it without pool_mutex from the worker thread is safe.
 */
#define MD_NOSTR_MAX_INSTANCES 16

static struct {
    pthread_mutex_t  lock;
    struct {
        NostrSimplePool *pool;
        MdNostr         *ctx;
    } entries[MD_NOSTR_MAX_INSTANCES];
    size_t count;
} g_registry = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .count = 0,
};

static void registry_add(NostrSimplePool *pool, MdNostr *ctx) {
    pthread_mutex_lock(&g_registry.lock);
    if (g_registry.count < MD_NOSTR_MAX_INSTANCES) {
        g_registry.entries[g_registry.count].pool = pool;
        g_registry.entries[g_registry.count].ctx  = ctx;
        g_registry.count++;
    } else {
        fprintf(stderr, "nostr: WARNING — instance registry full (%d)\n",
                MD_NOSTR_MAX_INSTANCES);
    }
    pthread_mutex_unlock(&g_registry.lock);
}

static void registry_remove(MdNostr *ctx) {
    pthread_mutex_lock(&g_registry.lock);
    for (size_t i = 0; i < g_registry.count; i++) {
        if (g_registry.entries[i].ctx == ctx) {
            for (size_t j = i; j + 1 < g_registry.count; j++)
                g_registry.entries[j] = g_registry.entries[j + 1];
            g_registry.count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_registry.lock);
}

static MdNostr *registry_find_by_relay(NostrRelay *relay) {
    MdNostr *found = NULL;
    pthread_mutex_lock(&g_registry.lock);
    for (size_t i = 0; i < g_registry.count; i++) {
        MdNostr *ctx = g_registry.entries[i].ctx;
        for (size_t j = 0; j < ctx->relay_count; j++) {
            if (ctx->relays[j] == relay) {
                found = ctx;
                goto done;
            }
        }
    }
done:
    pthread_mutex_unlock(&g_registry.lock);
    return found;
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

    /* Step 3: Decrypt seal content → rumor JSON */
    char *rumor_json = NULL;
    ret = md_signer_nip44_decrypt(ua->signer, sender_pk,
                                  seal_content, &rumor_json);
    nostr_event_free(seal);
    if (ret != 0 || !rumor_json) goto done;

    /* Step 4: Parse rumor to get DM content */
    NostrEvent *rumor = event_from_json(rumor_json);
    free(rumor_json);
    if (!rumor) goto done;

    const char *dm_content = nostr_event_get_content(rumor);
    if (dm_content && sender_pk) {
        char *sender_copy = strdup(sender_pk);
        char *content_copy = strdup(dm_content);
        nostr_event_free(rumor);

        if (sender_copy && content_copy)
            ua->on_dm(sender_copy, content_copy, ua->dm_userdata);

        free(sender_copy);
        free(content_copy);
    } else {
        nostr_event_free(rumor);
    }

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
static void md_nostr_event_handler(NostrIncomingEvent *incoming) {
    if (!incoming || !incoming->event)
        return;

    MdNostr *n = registry_find_by_relay(incoming->relay);
    if (!n)
        return;
    NostrEvent *ev = incoming->event;

    /* Application-layer dedup: skip events we've already processed.
     * This is defense-in-depth over nostrc's pool-level dedup ring. */
    const char *ev_id = nostr_event_get_id(ev);
    if (ev_id && dedup_is_seen(n, ev_id))
        return;

    int kind = nostr_event_get_kind(ev);

    if (kind == 1059 && n->cbs.on_dm && n->signer) {
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
    } else if (kind == 30078 && n->cbs.on_transport) {
        /* Transport address event — extract content (FIPS addr) */
        const char *pubkey = nostr_event_get_pubkey(ev);
        const char *content = nostr_event_get_content(ev);
        if (pubkey && content)
            n->cbs.on_transport(pubkey, content, n->cbs.transport_userdata);
    } else if (kind == 30000) {
        /* NIP-51 categorized people list — check if it's our allowlist.
         * Parse public entries (no sk_hex for private entry decryption
         * through the signer abstraction in Phase 2.1). */
        NostrList *list = nostr_nip51_parse_list(ev, NULL);
        if (list && list->identifier &&
            strcmp(list->identifier, "metadesk-allowlist") == 0) {
            /* Replace cached allowlist */
            if (n->allowlist)
                nostr_nip51_list_free(n->allowlist);
            n->allowlist = list;
            fprintf(stderr, "nostr: refreshed allowlist (%zu entries)\n",
                    list->count);
        } else {
            nostr_nip51_list_free(list);
        }
    }
}

/* ── NIP-11 relay capability probing ──────────────────────────
 * Fetch the NIP-11 relay information document for each relay URL.
 * This is an HTTPS GET to the same URL (with wss:// → https://).
 *
 * We log capabilities and warn about:
 *   - auth_required relays (NIP-42 handler must be wired up)
 *   - relays that don't list NIP-17 in supported_nips
 *   - max_content_length or max_message_length limits
 *
 * Each probe runs as a goroutine; callers use GoWaitGroup to wait
 * for all probes to complete. Startup latency is O(max_latency)
 * instead of O(N * latency).
 */

static void probe_relay_nip11_impl(const char *wss_url);

typedef struct {
    const char  *wss_url;
    GoWaitGroup *wg;
} Nip11ProbeArg;

static void *probe_relay_nip11_thread(void *arg) {
    Nip11ProbeArg *pa = arg;
    const char *wss_url = pa->wss_url;
    GoWaitGroup *wg = pa->wg;
    free(pa);

    probe_relay_nip11_impl(wss_url);
    go_wait_group_done(wg);
    return NULL;
}

static void probe_relay_nip11_impl(const char *wss_url) {
    if (!wss_url) return;

    /* Convert wss:// to https:// (or ws:// to http://) for NIP-11 */
    char http_url[512];
    if (strncmp(wss_url, "wss://", 6) == 0) {
        snprintf(http_url, sizeof(http_url), "https://%s", wss_url + 6);
    } else if (strncmp(wss_url, "ws://", 5) == 0) {
        snprintf(http_url, sizeof(http_url), "http://%s", wss_url + 5);
    } else {
        snprintf(http_url, sizeof(http_url), "%s", wss_url);
    }

    RelayInformationDocument *info = nostr_nip11_fetch_info(http_url);
    if (!info) {
        fprintf(stderr, "nostr: NIP-11 probe failed for %s (relay may not support NIP-11)\n",
                wss_url);
        return;
    }

    /* Log relay identity */
    fprintf(stderr, "nostr: NIP-11 %s — %s",
            wss_url, info->name ? info->name : "(unnamed)");
    if (info->software)
        fprintf(stderr, " [%s%s%s]",
                info->software,
                info->version ? " " : "",
                info->version ? info->version : "");
    fprintf(stderr, "\n");

    /* Check supported NIPs */
    bool has_nip17 = false;
    bool has_nip42 = false;
    if (info->supported_nips && info->supported_nips_count > 0) {
        fprintf(stderr, "nostr:   supported NIPs:");
        for (int i = 0; i < info->supported_nips_count; i++) {
            fprintf(stderr, " %d", info->supported_nips[i]);
            if (info->supported_nips[i] == 17) has_nip17 = true;
            if (info->supported_nips[i] == 42) has_nip42 = true;
        }
        fprintf(stderr, "\n");
    }

    if (!has_nip17) {
        fprintf(stderr, "nostr:   WARNING: relay does not advertise NIP-17 "
                "(gift-wrapped DMs may not be supported)\n");
    }

    /* Check limitations */
    if (info->limitation) {
        if (info->limitation->auth_required) {
            fprintf(stderr, "nostr:   NOTE: relay requires NIP-42 authentication%s\n",
                    has_nip42 ? " (handler installed)" : " (WARNING: NIP-42 not advertised)");
        }
        if (info->limitation->max_content_length > 0) {
            fprintf(stderr, "nostr:   max content length: %d bytes\n",
                    info->limitation->max_content_length);
        }
        if (info->limitation->max_message_length > 0) {
            fprintf(stderr, "nostr:   max message length: %d bytes\n",
                    info->limitation->max_message_length);
        }
        if (info->limitation->payment_required) {
            fprintf(stderr, "nostr:   WARNING: relay requires payment\n");
        }
    }

    nostr_nip11_free_info(info);
}

/* ── Lifecycle ────────────────────────────────────────────── */

MdNostr *md_nostr_create(const MdNostrConfig *cfg, const MdNostrCallbacks *cbs) {
    if (!cfg || !cfg->relay_urls || cfg->relay_count <= 0)
        return NULL;

    /* Must have either signer or sk_hex */
    if (!cfg->signer && !cfg->sk_hex)
        return NULL;

    MdNostr *n = calloc(1, sizeof(MdNostr));
    if (!n) return NULL;

    pthread_mutex_init(&n->dedup_mu, NULL);
    go_wait_group_init(&n->dm_wg);

    /* Set up signer: use provided signer, or create direct-key from sk_hex */
    if (cfg->signer) {
        n->signer = cfg->signer;
        n->owns_signer = false;
    } else {
        n->signer = md_signer_create_direct(cfg->sk_hex);
        if (!n->signer) {
            fprintf(stderr, "nostr: failed to create signer from sk_hex\n");
            free(n);
            return NULL;
        }
        n->owns_signer = true;
    }

    /* Get pubkey from signer */
    char *pk = NULL;
    if (md_signer_get_pubkey(n->signer, &pk) != MD_SIGNER_OK || !pk) {
        fprintf(stderr, "nostr: failed to get pubkey from signer\n");
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

    /* Register instance in static registry for event routing */
    registry_add(n->pool, n);

    /* Register event middleware for incoming event routing */
    nostr_simple_pool_set_event_middleware(n->pool, md_nostr_event_handler);

    /* Add all relay URLs — pool manages reconnection with backoff */
    for (int i = 0; i < cfg->relay_count; i++) {
        nostr_simple_pool_ensure_relay(n->pool, cfg->relay_urls[i]);
    }

    /* Probe each relay's NIP-11 info document for capability detection.
     * Fan-out: each probe runs concurrently via go(), collected by WaitGroup.
     * Startup latency = max(per-relay latency) instead of sum. */
    {
        GoWaitGroup wg;
        go_wait_group_init(&wg);
        for (int i = 0; i < cfg->relay_count; i++) {
            go_wait_group_add(&wg, 1);
            Nip11ProbeArg *pa = malloc(sizeof(Nip11ProbeArg));
            if (pa) {
                pa->wss_url = cfg->relay_urls[i];
                pa->wg = &wg;
                go(probe_relay_nip11_thread, pa);
            } else {
                go_wait_group_done(&wg);  /* balance the add */
            }
        }
        go_wait_group_wait(&wg);
        go_wait_group_destroy(&wg);
    }

    /* Snapshot relay pointers and URLs from pool.
     * This is the ONLY place we access pool struct internals.
     * TODO: Replace with nostr_simple_pool_get_relays() when nostrc
     * provides an accessor API. */
    n->relay_count = (size_t)cfg->relay_count;
    n->relays = malloc(n->relay_count * sizeof(NostrRelay *));
    n->relay_urls = malloc(n->relay_count * sizeof(char *));
    if (n->relays && n->relay_urls) {
        pthread_mutex_lock(&n->pool->pool_mutex);
        for (size_t i = 0; i < n->relay_count && i < n->pool->relay_count; i++) {
            n->relays[i] = n->pool->relays[i];
            n->relay_urls[i] = strdup(nostr_relay_get_url_const(n->pool->relays[i]));
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

    /* Start pool worker threads */
    nostr_simple_pool_start(n->pool);

    /* Subscribe to incoming NIP-17 gift-wrapped DMs (kind:1059) addressed
     * to our pubkey.  This is the live subscription that allows the host
     * and client to receive session-negotiation DMs without polling.
     * Events arrive via md_nostr_event_handler() → cbs.on_dm.
     *
     * since = now - 300s: catch DMs that arrived in the last 5 minutes
     * while we were offline, but avoid replaying ancient gift-wraps. */
    if (n->cbs.on_dm) {
        NostrFilter *f = nostr_filter_builder_build(
            nostr_filter_builder_since(
                nostr_filter_builder_tag(
                    nostr_filter_builder_kinds(
                        nostr_filter_builder_new(),
                        1059, -1),
                    "p", n->pk_hex),
                (int64_t)time(NULL) - 300));
        if (f) {
            NostrFilters *filters = nostr_filters_new();
            if (filters) {
                nostr_filters_add(filters, f);
                nostr_filter_free(f);
                nostr_simple_pool_subscribe(n->pool,
                                            (const char **)n->relay_urls,
                                            n->relay_count, *filters, true);
                nostr_filters_free(filters);
                fprintf(stderr, "nostr: subscribed to kind:1059 gift-wraps "
                                "for %.8s...\n", n->pk_hex);
            } else {
                nostr_filter_free(f);
            }
        }
    }

    fprintf(stderr, "nostr: bridge ready (signer=%s, pk=%.*s..., relays=%d)\n",
            md_signer_type_name(md_signer_get_type(n->signer)),
            8, n->pk_hex, cfg->relay_count);

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

    /* Remove from instance registry */
    registry_remove(n);

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

    /* Free dedup ring */
    for (size_t i = 0; i < MD_DEDUP_RING_CAP; i++)
        free(n->dedup_ids[i]);
    pthread_mutex_destroy(&n->dedup_mu);

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

/* ── Session signaling (NIP-17 gift-wrapped DMs) ──────────────
 *
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

    /* Step 1: Build rumor (kind:14, unsigned) */
    NostrEvent *rumor = nostr_event_new();
    if (!rumor) return -1;

    nostr_event_set_kind(rumor, 14);
    nostr_event_set_content(rumor, content);
    nostr_event_set_pubkey(rumor, n->pk_hex);
    nostr_event_set_created_at(rumor, (int64_t)time(NULL));
    /* Rumor is NOT signed per NIP-17 */

    char *rumor_json = nostr_event_serialize(rumor);
    nostr_event_free(rumor);
    if (!rumor_json) return -1;

    /* Step 2: NIP-44 encrypt rumor → seal content */
    char *encrypted_rumor = NULL;
    int ret = md_signer_nip44_encrypt(n->signer, recipient_pubkey_hex,
                                      rumor_json, &encrypted_rumor);
    free(rumor_json);
    if (ret != MD_SIGNER_OK || !encrypted_rumor)
        return -1;

    /* Step 3: Build seal (kind:13) and sign with our key */
    NostrEvent *seal = nostr_event_new();
    if (!seal) { free(encrypted_rumor); return -1; }

    nostr_event_set_kind(seal, 13);
    nostr_event_set_content(seal, encrypted_rumor);
    nostr_event_set_pubkey(seal, n->pk_hex);
    nostr_event_set_created_at(seal, (int64_t)time(NULL));
    free(encrypted_rumor);

    /* Sign seal via signer */
    NostrEvent *signed_seal = sign_event_via_signer(n->signer, seal);
    nostr_event_free(seal);
    if (!signed_seal) return -1;

    char *seal_json = nostr_event_serialize(signed_seal);
    nostr_event_free(signed_seal);
    if (!seal_json) return -1;

    /* Step 4: Create ephemeral key for gift-wrap */
    char *eph_sk = nostr_key_generate_private();
    if (!eph_sk) { free(seal_json); return -1; }

    char *eph_pk = nostr_key_get_public(eph_sk);
    if (!eph_pk) {
        memset(eph_sk, 0, strlen(eph_sk));
        free(eph_sk);
        free(seal_json);
        return -1;
    }

    /* Step 5: NIP-44 encrypt seal with ephemeral key → gift-wrap content */
    uint8_t eph_sk_bytes[32], recip_pk_bytes[32];
    if (hex_to_bytes(eph_sk, eph_sk_bytes, 32) != 0 ||
        hex_to_bytes(recipient_pubkey_hex, recip_pk_bytes, 32) != 0) {
        memset(eph_sk, 0, strlen(eph_sk));
        free(eph_sk);
        free(eph_pk);
        free(seal_json);
        return -1;
    }
    char *encrypted_seal = NULL;
    ret = nostr_nip44_encrypt_v2(eph_sk_bytes, recip_pk_bytes,
                                 (const uint8_t *)seal_json, strlen(seal_json),
                                 &encrypted_seal);
    memset(eph_sk_bytes, 0, sizeof(eph_sk_bytes));
    free(seal_json);
    if (ret != 0 || !encrypted_seal) {
        memset(eph_sk, 0, strlen(eph_sk));
        free(eph_sk);
        free(eph_pk);
        return -1;
    }

    /* Step 6: Build gift-wrap (kind:1059), sign with ephemeral key */
    NostrEvent *gift_wrap = nostr_event_new();
    if (!gift_wrap) {
        free(encrypted_seal);
        memset(eph_sk, 0, strlen(eph_sk));
        free(eph_sk);
        free(eph_pk);
        return -1;
    }

    nostr_event_set_kind(gift_wrap, 1059);
    nostr_event_set_content(gift_wrap, encrypted_seal);
    nostr_event_set_pubkey(gift_wrap, eph_pk);
    nostr_event_set_created_at(gift_wrap, (int64_t)time(NULL));
    free(encrypted_seal);

    /* Sign gift-wrap with ephemeral key (local, not via signer) */
    nostr_event_sign(gift_wrap, eph_sk);

    /* Zero and free ephemeral key */
    memset(eph_sk, 0, strlen(eph_sk));
    free(eph_sk);
    free(eph_pk);

    /* Step 7: Publish gift-wrap to all connected relays */
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

bool md_nostr_is_allowed(MdNostr *n, const char *pubkey_hex) {
    if (!n || !pubkey_hex)
        return false;

    if (!n->allowlist)
        return false;

    for (size_t i = 0; i < n->allowlist->count; i++) {
        NostrListEntry *entry = n->allowlist->entries[i];
        if (entry && entry->tag_name && strcmp(entry->tag_name, "p") == 0
            && entry->value && strcmp(entry->value, pubkey_hex) == 0) {
            return true;
        }
    }
    return false;
}

bool md_nostr_has_allowlist(const MdNostr *n) {
    return n && n->allowlist && n->allowlist->count > 0;
}

int md_nostr_refresh_allowlist(MdNostr *n) {
    if (!n || !n->pool || !n->pk_hex) return -1;

    /* Subscribe to kind:30000, authors:[our_pk], #d:["metadesk-allowlist"].
     * limit:1 — this is an addressable event (NIP-33) so only the latest
     * version exists; the subscription stays open for live replacements. */
    NostrFilter *f = nostr_filter_builder_build(
        nostr_filter_builder_limit(
            nostr_filter_builder_tag(
                nostr_filter_builder_authors(
                    nostr_filter_builder_kinds(
                        nostr_filter_builder_new(),
                        30000, -1),
                    n->pk_hex, NULL),
                "d", "metadesk-allowlist"),
            1));
    if (!f) return -1;

    NostrFilters *filters = nostr_filters_new();
    if (!filters) { nostr_filter_free(f); return -1; }
    nostr_filters_add(filters, f);
    nostr_filter_free(f);

    /* Use cached relay URLs for subscription */
    nostr_simple_pool_subscribe(n->pool, (const char **)n->relay_urls,
                                n->relay_count, *filters, true);

    nostr_filters_free(filters);

    fprintf(stderr, "nostr: subscribed to allowlist updates\n");
    return 0;
}

int md_nostr_allowlist_add(MdNostr *n, const char *pubkey_hex, const char *caps) {
    if (!n || !n->signer || !pubkey_hex)
        return -1;

    if (!n->allowlist)
        n->allowlist = nostr_nip51_list_new();
    if (!n->allowlist)
        return -1;

    NostrListEntry *entry = nostr_nip51_entry_new("p", pubkey_hex, caps, false);
    if (!entry)
        return -1;

    nostr_nip51_list_add_entry(n->allowlist, entry);
    nostr_nip51_list_set_identifier(n->allowlist, "metadesk-allowlist");

    /* Build list event (kind:30000) with entries as tags */
    NostrEvent *list_event = nostr_event_new();
    if (!list_event) return -1;

    nostr_event_set_kind(list_event, 30000);
    nostr_event_set_pubkey(list_event, n->pk_hex);
    nostr_event_set_created_at(list_event, (int64_t)time(NULL));

    /* Serialize allowlist entries as NIP-51 tags:
     *   ["d", "metadesk-allowlist"]  — addressable d-tag
     *   ["p", "<pubkey>", "<caps>"]  — one per allowlist entry */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) { nostr_event_free(list_event); return -1; }

    /* d-tag */
    NostrTag *d_tag = nostr_tag_new("d", "metadesk-allowlist", NULL);
    if (d_tag) nostr_tags_append(tags, d_tag);

    /* Entry tags */
    for (size_t i = 0; i < n->allowlist->count; i++) {
        NostrListEntry *e = n->allowlist->entries[i];
        if (!e || !e->tag_name || !e->value) continue;
        NostrTag *t = e->extra
            ? nostr_tag_new(e->tag_name, e->value, e->extra, NULL)
            : nostr_tag_new(e->tag_name, e->value, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    nostr_event_set_tags(list_event, tags); /* takes ownership */

    /* Sign via signer abstraction */
    NostrEvent *signed_event = sign_event_via_signer(n->signer, list_event);
    nostr_event_free(list_event);
    if (!signed_event) return -1;

    /* Publish to all connected relays */
    int ret = publish_all(n, signed_event);
    nostr_event_free(signed_event);

    if (ret == 0) {
        fprintf(stderr, "nostr: published allowlist (%zu entries)\n",
                n->allowlist->count);
    }

    return ret;
}

int md_nostr_allowlist_remove(MdNostr *n, const char *pubkey_hex) {
    if (!n || !pubkey_hex || !n->allowlist)
        return -1;

    /* Find and remove the entry matching pubkey_hex */
    bool found = false;
    for (size_t i = 0; i < n->allowlist->count; i++) {
        NostrListEntry *entry = n->allowlist->entries[i];
        if (entry && entry->tag_name && strcmp(entry->tag_name, "p") == 0
            && entry->value && strcmp(entry->value, pubkey_hex) == 0) {
            /* Remove by shifting remaining entries down */
            nostr_nip51_entry_free(entry);
            for (size_t j = i; j + 1 < n->allowlist->count; j++)
                n->allowlist->entries[j] = n->allowlist->entries[j + 1];
            n->allowlist->count--;
            found = true;
            break;
        }
    }

    if (!found)
        return -1;

    /* Republish the updated allowlist by rebuilding the kind:30000 event */
    NostrEvent *list_event = nostr_event_new();
    if (!list_event) return -1;

    nostr_event_set_kind(list_event, 30000);
    nostr_event_set_pubkey(list_event, n->pk_hex);
    nostr_event_set_created_at(list_event, (int64_t)time(NULL));

    NostrTags *tags = nostr_tags_new(0);
    if (!tags) { nostr_event_free(list_event); return -1; }

    NostrTag *d_tag = nostr_tag_new("d", "metadesk-allowlist", NULL);
    if (d_tag) nostr_tags_append(tags, d_tag);

    for (size_t i = 0; i < n->allowlist->count; i++) {
        NostrListEntry *e = n->allowlist->entries[i];
        if (!e || !e->tag_name || !e->value) continue;
        NostrTag *t = e->extra
            ? nostr_tag_new(e->tag_name, e->value, e->extra, NULL)
            : nostr_tag_new(e->tag_name, e->value, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    nostr_event_set_tags(list_event, tags);

    NostrEvent *signed_event = sign_event_via_signer(n->signer, list_event);
    nostr_event_free(list_event);
    if (!signed_event) return -1;

    int ret = publish_all(n, signed_event);
    nostr_event_free(signed_event);

    if (ret == 0) {
        fprintf(stderr, "nostr: published updated allowlist (%zu entries)\n",
                n->allowlist->count);
    }

    return ret;
}

int md_nostr_allowlist_count(const MdNostr *n) {
    if (!n || !n->allowlist) return 0;
    return (int)n->allowlist->count;
}

int md_nostr_allowlist_get_entry(const MdNostr *n, int index,
                                 MdAllowlistEntry *out) {
    if (!n || !out || !n->allowlist || index < 0 ||
        (size_t)index >= n->allowlist->count)
        return -1;

    NostrListEntry *entry = n->allowlist->entries[index];
    if (!entry) return -1;

    out->pubkey_hex = entry->value;
    out->caps = entry->extra;
    return 0;
}

/* ── Transport address ────────────────────────────────────────
 *
 * kind:30078 is addressable, d-tag is "fips-transport".
 */

int md_nostr_publish_transport(MdNostr *n, const char *fips_addr) {
    if (!n || !n->signer || !fips_addr)
        return -1;

    NostrEvent *event = nostr_event_new();
    if (!event) return -1;

    nostr_event_set_kind(event, 30078);
    nostr_event_set_content(event, fips_addr);
    nostr_event_set_pubkey(event, n->pk_hex);
    nostr_event_set_created_at(event, (int64_t)time(NULL));

    /* Add d-tag ["d", "fips-transport"] — makes this an addressable event (NIP-33) */
    NostrTag *d_tag = nostr_tag_new("d", "fips-transport", NULL);
    if (!d_tag) { nostr_event_free(event); return -1; }
    NostrTags *tags = nostr_tags_new(1, d_tag);
    if (!tags) { nostr_tag_free(d_tag); nostr_event_free(event); return -1; }
    nostr_event_set_tags(event, tags); /* takes ownership */

    /* Sign via signer abstraction */
    NostrEvent *signed_event = sign_event_via_signer(n->signer, event);
    nostr_event_free(event);
    if (!signed_event) return -1;

    /* Publish to all connected relays */
    int ret = publish_all(n, signed_event);
    nostr_event_free(signed_event);

    if (ret == 0) {
        fprintf(stderr, "nostr: published transport addr %s\n", fips_addr);
    }

    return ret;
}

int md_nostr_subscribe_transport(MdNostr *n, const char *host_pubkey_hex) {
    if (!n || !n->pool || !host_pubkey_hex)
        return -1;

    /* Build filter: kind:30078, authors:[host_pubkey], #d:["fips-transport"].
     * limit:1 — addressable event (NIP-33), only the latest version exists;
     * subscription stays open for live replacements. */
    NostrFilter *f = nostr_filter_builder_build(
        nostr_filter_builder_limit(
            nostr_filter_builder_tag(
                nostr_filter_builder_authors(
                    nostr_filter_builder_kinds(
                        nostr_filter_builder_new(),
                        30078, -1),
                    host_pubkey_hex, NULL),
                "d", "fips-transport"),
            1));
    if (!f) return -1;

    /* Wrap in NostrFilters (by-value struct for pool_subscribe) */
    NostrFilters *filters = nostr_filters_new();
    if (!filters) { nostr_filter_free(f); return -1; }
    nostr_filters_add(filters, f);
    /* f contents moved into filters; f is zeroed but still needs freeing */
    nostr_filter_free(f);

    /* Use cached relay URLs for subscription.
     * Incoming kind:30078 events route through
     * md_nostr_event_handler() which calls cbs.on_transport */
    nostr_simple_pool_subscribe(n->pool, (const char **)n->relay_urls,
                                n->relay_count, *filters, true);

    nostr_filters_free(filters);

    fprintf(stderr, "nostr: subscribed to transport addr for %.8s...\n",
            host_pubkey_hex);
    return 0;
}
