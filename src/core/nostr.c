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
 * All Nostr primitives (events, keys, encryption, relay I/O) are
 * provided by nostrc (github.com/chebizarro/nostrc).
 */
#include "nostr.h"
#include <stdlib.h>
#include <string.h>

struct MdNostr {
    char              *sk_hex;       /* secret key, hex (mlock'd in production) */
    char              *pk_hex;       /* public key, hex                         */
    NostrSimplePool   *pool;         /* nostrc relay pool — persistent conns    */
    NostrList         *allowlist;    /* cached NIP-51 allowlist                 */
    MdNostrCallbacks   cbs;          /* event callbacks                         */
};

/* ── Event middleware ─────────────────────────────────────────
 * nostrc's NostrSimplePool invokes the event_middleware callback
 * for every incoming event across all subscriptions. We route
 * events to the appropriate metadesk callback based on kind.
 *
 * This runs on the pool's worker thread — callbacks must be
 * thread-safe or dispatch to the main thread.
 */
/* File-scope context pointer for event middleware routing.
 * nostrc's NostrSimplePool middleware doesn't carry user_data,
 * so we store the MdNostr* here. This limits us to a single
 * MdNostr instance per process (which is the intended usage).
 * Will be replaced when nostrc adds user_data to middleware. */
static MdNostr *g_nostr_ctx = NULL;

static void md_nostr_event_handler(NostrIncomingEvent *incoming) {
    if (!incoming || !incoming->event || !g_nostr_ctx)
        return;

    MdNostr *n = g_nostr_ctx;
    NostrEvent *ev = incoming->event;
    int kind = nostr_event_get_kind(ev);

    if (kind == 1059 && n->cbs.on_dm) {
        /* NIP-17 gift-wrap: unwrap → unseal → get rumor content.
         * TODO: call nostr_nip17_unwrap_dm(n->sk_hex, ev) and
         * invoke on_dm with decrypted content + sender pubkey.
         * Requires nostrc NIP-17 unwrap API. */
    } else if (kind == 30078 && n->cbs.on_transport) {
        /* Transport address event — extract content (FIPS addr) */
        const char *pubkey = nostr_event_get_pubkey(ev);
        const char *content = nostr_event_get_content(ev);
        if (pubkey && content)
            n->cbs.on_transport(pubkey, content, n->cbs.transport_userdata);
    }
}

/* ── Lifecycle ────────────────────────────────────────────── */

MdNostr *md_nostr_create(const MdNostrConfig *cfg, const MdNostrCallbacks *cbs) {
    if (!cfg || !cfg->sk_hex || !cfg->relay_urls || cfg->relay_count <= 0)
        return NULL;

    MdNostr *n = calloc(1, sizeof(MdNostr));
    if (!n) return NULL;

    /* Store key material */
    n->sk_hex = strdup(cfg->sk_hex);
    if (!n->sk_hex) {
        free(n);
        return NULL;
    }
    n->pk_hex = nostr_key_get_public(cfg->sk_hex);
    if (!n->pk_hex) {
        free(n->sk_hex);
        free(n);
        return NULL;
    }

    /* Store callbacks */
    if (cbs)
        n->cbs = *cbs;

    /* Create relay pool — persistent WebSocket connections */
    n->pool = nostr_simple_pool_new();
    if (!n->pool) {
        free(n->pk_hex);
        free(n->sk_hex);
        free(n);
        return NULL;
    }

    /* Store file-scope context for event middleware routing */
    g_nostr_ctx = n;

    /* Register event middleware for incoming event routing */
    nostr_simple_pool_set_event_middleware(n->pool, md_nostr_event_handler);

    /* Add all relay URLs — pool manages reconnection with backoff */
    for (int i = 0; i < cfg->relay_count; i++) {
        nostr_simple_pool_ensure_relay(n->pool, cfg->relay_urls[i]);
    }

    /* Start pool worker threads — relay connections are established
     * and maintained from this point until md_nostr_destroy(). */
    nostr_simple_pool_start(n->pool);

    /* Register live subscriptions:
     *
     * 1. kind:1059 (NIP-17 gift-wraps) addressed to our pubkey.
     *    These are session request/accept DMs. The subscription
     *    persists for the app lifetime. Relay sends stored events
     *    first, then EOSE, then new events in real-time.
     *
     * 2. kind:30078 (transport address) — subscribed on demand
     *    via md_nostr_subscribe_transport().
     *
     * TODO: Build NostrFilters for kinds=[1059], #p=[our_pk_hex]
     * and call nostr_simple_pool_subscribe() with all relay URLs.
     */

    return n;
}

const char *md_nostr_get_npub(const MdNostr *n) {
    return n ? n->pk_hex : NULL;
}

void md_nostr_destroy(MdNostr *n) {
    if (!n) return;

    /* Clear file-scope context if it points to us */
    if (g_nostr_ctx == n)
        g_nostr_ctx = NULL;

    /* Stop pool — closes all subscriptions (sends CLOSE to relays),
     * disconnects all WebSocket connections, drains worker queues. */
    if (n->pool) {
        nostr_simple_pool_stop(n->pool);
        nostr_simple_pool_free(n->pool);
    }
    if (n->allowlist)
        nostr_nip51_list_free(n->allowlist);

    /* Zero secret key before freeing */
    if (n->sk_hex) {
        memset(n->sk_hex, 0, strlen(n->sk_hex));
        free(n->sk_hex);
    }
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

/* ── Session signaling (NIP-17 gift-wrapped DMs) ──────────────
 *
 * Session messages use the NIP-17 three-layer encryption:
 *   1. Rumor (kind:14) — unsigned message with session JSON
 *   2. Seal (kind:13) — signed, NIP-44 encrypted to recipient
 *   3. Gift-wrap (kind:1059) — ephemeral sender, encrypted seal
 *
 * The recipient has a live subscription for kind:1059 events
 * addressed to their pubkey. When a gift-wrap arrives, the pool's
 * event middleware fires, we unwrap it via NIP-17/NIP-44, and
 * invoke the on_dm callback with the decrypted content.
 *
 * Publishing is fire-and-forget: we hand the gift-wrap event to
 * every relay in the pool. The relay confirms receipt with an
 * OK message (handled internally by nostrc).
 */

static int md_nostr_send_dm(MdNostr *n, const char *recipient_pubkey_hex,
                            const char *content) {
    if (!n || !recipient_pubkey_hex || !content)
        return -1;

    /* Create three-layer gift-wrapped DM via nostrc NIP-17 */
    NostrEvent *gift_wrap = nostr_nip17_wrap_dm(
        n->sk_hex, recipient_pubkey_hex, content);
    if (!gift_wrap)
        return -1;

    /* Publish to all connected relays.
     * nostrc pool iterates relays and sends ["EVENT", <gift_wrap_json>].
     * Each relay responds with ["OK", event_id, true/false, reason].
     * Pool handles OK routing internally. */
    /* Publish gift-wrap to all connected relays.
     * TODO: nostr_simple_pool_publish() may not exist yet;
     * iterate pool relays individually if needed. */
    int ret = nostr_simple_pool_publish(n->pool, gift_wrap);

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

/* ── Access control (NIP-51 allowlists) ───────────────────────
 *
 * The host's allowlist is a kind:30000 addressable event (NIP-51)
 * with d-tag "metadesk-allowlist". It's a replaceable event —
 * only the latest version for this (pubkey, kind, d-tag) tuple
 * is retained by relays.
 *
 * We cache the parsed list locally. refresh_allowlist() subscribes
 * to the relay, collects the latest version (relay sends stored
 * events then EOSE), and updates the cache.
 */

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

int md_nostr_refresh_allowlist(MdNostr *n) {
    if (!n) return -1;

    /* TODO: Subscribe to kind:30000, authors:[our_pk], #d:["metadesk-allowlist"].
     * Wait for EOSE (nostrc signals this via subscription lifecycle).
     * Parse the latest event with nostr_nip51_parse_list().
     * Replace n->allowlist with the parsed result. */
    return 0;
}

int md_nostr_allowlist_add(MdNostr *n, const char *pubkey_hex, const char *caps) {
    if (!n || !pubkey_hex)
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

    /* Publish updated list — kind:30000 is addressable, so relays
     * replace the old version automatically. */
    NostrEvent *list_event = nostr_nip51_create_list(30000, n->allowlist, n->sk_hex);
    if (!list_event)
        return -1;

    /* TODO: publish to all relays via pool */
    nostr_event_free(list_event);
    return 0;
}

int md_nostr_allowlist_remove(MdNostr *n, const char *pubkey_hex) {
    if (!n || !pubkey_hex || !n->allowlist)
        return -1;

    /* TODO: rebuild allowlist without the target pubkey, republish */
    return 0;
}

/* ── Transport address ────────────────────────────────────────
 *
 * kind:30078 is addressable (30000 <= kind < 40000), meaning
 * relays keep only the latest event per (pubkey, kind, d-tag).
 * d-tag is "fips-transport".
 *
 * Publishing: host calls md_nostr_publish_transport() which creates
 * a signed kind:30078 event and publishes to all pool relays.
 *
 * Subscribing: client calls md_nostr_subscribe_transport() which
 * sends REQ with filter {kinds:[30078], authors:[host_pk], #d:["fips-transport"]}.
 * The relay sends the latest stored event, then EOSE, then pushes
 * any future updates in real-time. Results arrive via on_transport callback.
 */

int md_nostr_publish_transport(MdNostr *n, const char *fips_addr) {
    if (!n || !fips_addr)
        return -1;

    NostrEvent *event = nostr_event_new();
    if (!event) return -1;

    nostr_event_set_kind(event, 30078);
    nostr_event_set_content(event, fips_addr);
    nostr_event_set_pubkey(event, n->pk_hex);

    /* TODO: add d-tag ["d", "fips-transport"] via nostr_tags API */

    nostr_event_sign(event, n->sk_hex);

    /* TODO: publish to all pool relays */

    nostr_event_free(event);
    return 0;
}

int md_nostr_subscribe_transport(MdNostr *n, const char *host_pubkey_hex) {
    if (!n || !host_pubkey_hex)
        return -1;

    /* TODO: Build filter: kinds=[30078], authors=[host_pubkey_hex], #d=["fips-transport"]
     * Subscribe via nostr_simple_pool_subscribe().
     * Relay will send the latest stored event, then EOSE, then real-time updates.
     * Incoming events are routed through md_nostr_event_handler() → on_transport callback.
     */
    return 0;
}
