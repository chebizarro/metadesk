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

/* Helper: publish an event to all relays in the pool */
static int pool_publish_all(NostrSimplePool *pool, NostrEvent *ev) {
    if (!pool || !ev) return -1;
    int published = 0;
    pthread_mutex_lock(&pool->pool_mutex);
    for (size_t i = 0; i < pool->relay_count; i++) {
        nostr_relay_publish(pool->relays[i], ev);
        published++;
    }
    pthread_mutex_unlock(&pool->pool_mutex);
    return published > 0 ? 0 : -1;
}

struct MdNostr {
    MdSigner          *signer;       /* signing backend                     */
    bool               owns_signer;  /* true if we created it (from sk_hex) */
    char              *pk_hex;       /* our public key, hex                 */
    NostrSimplePool   *pool;         /* nostrc relay pool — persistent conns */
    NostrList         *allowlist;    /* cached NIP-51 allowlist             */
    MdNostrCallbacks   cbs;          /* event callbacks                     */
};

/* ── Event middleware ─────────────────────────────────────────
 * nostrc's NostrSimplePool invokes the event_middleware callback
 * for every incoming event across all subscriptions. We route
 * events to the appropriate metadesk callback based on kind.
 *
 * This runs on the pool's worker thread — callbacks must be
 * thread-safe or dispatch to the main thread.
 */
static MdNostr *g_nostr_ctx = NULL;

static void md_nostr_event_handler(NostrIncomingEvent *incoming) {
    if (!incoming || !incoming->event || !g_nostr_ctx)
        return;

    MdNostr *n = g_nostr_ctx;
    NostrEvent *ev = incoming->event;
    int kind = nostr_event_get_kind(ev);

    if (kind == 1059 && n->cbs.on_dm && n->signer) {
        /* NIP-17 gift-wrap: unwrap using signer.
         *
         * 1. NIP-44 decrypt gift-wrap content using our key
         *    (the gift-wrap was encrypted to our pubkey by an ephemeral key)
         * 2. Parse seal (kind:13)
         * 3. NIP-44 decrypt seal content using our key
         *    (the seal was encrypted by the sender to our pubkey)
         * 4. Parse rumor (kind:14), extract content + sender pubkey
         *
         * For direct-key signers, this is straightforward NIP-44 decrypt.
         * For remote signers, the signer daemon handles the decryption.
         */
        const char *ephemeral_pk = nostr_event_get_pubkey(ev);
        const char *gw_content = nostr_event_get_content(ev);
        if (!ephemeral_pk || !gw_content) return;

        /* Step 1: Decrypt gift-wrap → seal JSON */
        char *seal_json = NULL;
        int ret = md_signer_nip44_decrypt(n->signer, ephemeral_pk,
                                          gw_content, &seal_json);
        if (ret != 0 || !seal_json) return;

        /* Step 2: Parse seal to get sender pubkey and encrypted content */
        NostrEvent *seal = event_from_json(seal_json);
        free(seal_json);
        if (!seal) return;

        const char *sender_pk = nostr_event_get_pubkey(seal);
        const char *seal_content = nostr_event_get_content(seal);
        if (!sender_pk || !seal_content) {
            nostr_event_free(seal);
            return;
        }

        /* Step 3: Decrypt seal content → rumor JSON */
        char *rumor_json = NULL;
        ret = md_signer_nip44_decrypt(n->signer, sender_pk,
                                      seal_content, &rumor_json);
        nostr_event_free(seal);
        if (ret != 0 || !rumor_json) return;

        /* Step 4: Parse rumor to get DM content */
        NostrEvent *rumor = event_from_json(rumor_json);
        free(rumor_json);
        if (!rumor) return;

        const char *dm_content = nostr_event_get_content(rumor);
        if (dm_content && sender_pk) {
            /* Make copies since we're about to free the event */
            char *sender_copy = strdup(sender_pk);
            char *content_copy = strdup(dm_content);
            nostr_event_free(rumor);

            if (sender_copy && content_copy)
                n->cbs.on_dm(sender_copy, content_copy, n->cbs.dm_userdata);

            free(sender_copy);
            free(content_copy);
        } else {
            nostr_event_free(rumor);
        }
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

/* ── Lifecycle ────────────────────────────────────────────── */

MdNostr *md_nostr_create(const MdNostrConfig *cfg, const MdNostrCallbacks *cbs) {
    if (!cfg || !cfg->relay_urls || cfg->relay_count <= 0)
        return NULL;

    /* Must have either signer or sk_hex */
    if (!cfg->signer && !cfg->sk_hex)
        return NULL;

    MdNostr *n = calloc(1, sizeof(MdNostr));
    if (!n) return NULL;

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

    /* Store file-scope context for event middleware routing */
    g_nostr_ctx = n;

    /* Register event middleware for incoming event routing */
    nostr_simple_pool_set_event_middleware(n->pool, md_nostr_event_handler);

    /* Add all relay URLs — pool manages reconnection with backoff */
    for (int i = 0; i < cfg->relay_count; i++) {
        nostr_simple_pool_ensure_relay(n->pool, cfg->relay_urls[i]);
    }

    /* Start pool worker threads */
    nostr_simple_pool_start(n->pool);

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

    /* Clear file-scope context if it points to us */
    if (g_nostr_ctx == n)
        g_nostr_ctx = NULL;

    /* Stop pool — closes all subscriptions, disconnects relays */
    if (n->pool) {
        nostr_simple_pool_stop(n->pool);
        nostr_simple_pool_free(n->pool);
    }
    if (n->allowlist)
        nostr_nip51_list_free(n->allowlist);

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
    ret = pool_publish_all(n->pool, gift_wrap);
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

int md_nostr_refresh_allowlist(MdNostr *n) {
    if (!n || !n->pool || !n->pk_hex) return -1;

    /* Subscribe to kind:30000, authors:[our_pk], #d:["metadesk-allowlist"] */
    NostrFilter *f = nostr_filter_builder_build(
        nostr_filter_builder_tag(
            nostr_filter_builder_authors(
                nostr_filter_builder_kinds(
                    nostr_filter_builder_new(),
                    30000, -1),
                n->pk_hex, NULL),
            "d", "metadesk-allowlist"));
    if (!f) return -1;

    NostrFilters *filters = nostr_filters_new();
    if (!filters) { nostr_filter_free(f); return -1; }
    nostr_filters_add(filters, f);
    nostr_filter_free(f);

    /* Collect relay URLs */
    pthread_mutex_lock(&n->pool->pool_mutex);
    size_t relay_count = n->pool->relay_count;
    const char **urls = malloc(relay_count * sizeof(char *));
    if (!urls) {
        pthread_mutex_unlock(&n->pool->pool_mutex);
        nostr_filters_free(filters);
        return -1;
    }
    for (size_t i = 0; i < relay_count; i++)
        urls[i] = n->pool->relays[i]->url;

    nostr_simple_pool_subscribe(n->pool, urls, relay_count, *filters, true);
    pthread_mutex_unlock(&n->pool->pool_mutex);

    free(urls);
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
    int ret = pool_publish_all(n->pool, signed_event);
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
    /* TODO: rebuild allowlist without the target pubkey, republish */
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
    int ret = pool_publish_all(n->pool, signed_event);
    nostr_event_free(signed_event);

    if (ret == 0) {
        fprintf(stderr, "nostr: published transport addr %s\n", fips_addr);
    }

    return ret;
}

int md_nostr_subscribe_transport(MdNostr *n, const char *host_pubkey_hex) {
    if (!n || !n->pool || !host_pubkey_hex)
        return -1;

    /* Build filter: kind:30078, authors:[host_pubkey], #d:["fips-transport"] */
    NostrFilter *f = nostr_filter_builder_build(
        nostr_filter_builder_tag(
            nostr_filter_builder_authors(
                nostr_filter_builder_kinds(
                    nostr_filter_builder_new(),
                    30078, -1),
                host_pubkey_hex, NULL),
            "d", "fips-transport"));
    if (!f) return -1;

    /* Wrap in NostrFilters (by-value struct for pool_subscribe) */
    NostrFilters *filters = nostr_filters_new();
    if (!filters) { nostr_filter_free(f); return -1; }
    nostr_filters_add(filters, f);
    /* f contents moved into filters; f is zeroed but still needs freeing */
    nostr_filter_free(f);

    /* Collect relay URLs */
    pthread_mutex_lock(&n->pool->pool_mutex);
    size_t relay_count = n->pool->relay_count;
    const char **urls = malloc(relay_count * sizeof(char *));
    if (!urls) {
        pthread_mutex_unlock(&n->pool->pool_mutex);
        nostr_filters_free(filters);
        return -1;
    }
    for (size_t i = 0; i < relay_count; i++)
        urls[i] = n->pool->relays[i]->url;

    /* Subscribe — incoming kind:30078 events route through
     * md_nostr_event_handler() which calls cbs.on_transport */
    nostr_simple_pool_subscribe(n->pool, urls, relay_count, *filters, true);
    pthread_mutex_unlock(&n->pool->pool_mutex);

    free(urls);
    nostr_filters_free(filters);

    fprintf(stderr, "nostr: subscribed to transport addr for %.8s...\n",
            host_pubkey_hex);
    return 0;
}
