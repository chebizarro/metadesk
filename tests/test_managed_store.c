#include "managed_store.h"
#ifdef _WIN32
#include "md_test_win_private.h"
#endif
#include "md_time.h"
#include "nostr.h"
#include <nostr-simple-pool.h>
#include <nostr-kinds.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jansson.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
#define HOST "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define OTHER "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define PK "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
#define HOST_PK "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define TAG "metadesk-tag:86b09912-5e73-4bb0-a708-30c770882ace"
#define SECRET "Private note that must never hit disk"
#define V1 "{\"schema_version\":1,\"future\":{\"feature\":true},\"hosts\":{\"" HOST "\":{\"label\":\"Test host\",\"notes\":\"" SECRET "\",\"quality\":{\"max_width\":1280,\"max_height\":720,\"bitrate\":1000000,\"fps\":30},\"future_host\":true}}}"

static char *event_id(const char *wire) {
    NostrEvent *e = nostr_event_new();
    CHECK(nostr_event_deserialize_signed(e,wire,NULL) == NOSTR_EVENT_VALIDATION_OK);
    char *result = nostr_event_get_id(e);
    nostr_event_free(e);
    CHECK(result);
    return result;
}
static char *make(MdSigner *signer, const char *owner, const char *d, int kind, const char *body, int64_t at) {
    NostrEvent *e = md_private_document_build(signer,NULL,owner,kind,d,body,at);
    CHECK(e);
    char *wire = NULL;
    CHECK(!md_nostr_sign_event_json(signer,e,&wire));
    nostr_event_free(e);
    return wire;
}

typedef struct {
    MdManagedStore *store;
    pthread_mutex_t mu;
    pthread_cond_t changed;
    bool eose, closed;
    int64_t now;
    size_t received;
} History;
static void event(NostrPoolSubscription *sub, NostrIncomingEvent *incoming, void *ctx) {
    (void)sub;
    History *h = ctx;
    char *wire = nostr_event_serialize_compact(incoming->event);
    nostr_event_free(incoming->event);
    CHECK(wire);
    pthread_mutex_lock(&h->mu);
    MdStoreResult r = md_managed_store_receive(h->store,MD_STORE_MANAGEMENT,wire,h->now);
    CHECK(r == MD_STORE_OK || r == MD_STORE_UNCHANGED || r == MD_STORE_CONFLICT);
    ++h->received;
    pthread_mutex_unlock(&h->mu);
    free(wire);
}
static void state(NostrPoolSubscription *sub, NostrRelay *relay, const char *name,
    const char *reason, const NostrPoolCallbackInfo *info, void *ctx) {
    (void)sub; (void)relay; (void)reason; (void)info;
    History *h = ctx;
    pthread_mutex_lock(&h->mu);
    if (!strcmp(name,"EOSE")) {
        CHECK(md_managed_store_backfill(h->store,MD_STORE_MANAGEMENT,true) == MD_STORE_OK);
        h->eose = true;
    } else if (!strcmp(name,"CLOSED")) {
        CHECK(md_managed_store_backfill(h->store,MD_STORE_MANAGEMENT,false) == MD_STORE_OK);
        h->closed = true;
    }
    pthread_cond_broadcast(&h->changed);
    pthread_mutex_unlock(&h->mu);
}
static NostrPoolSubscription *subscribe(NostrSimplePool *pool, const char *url, History *h) {
    char *f = md_managed_store_filter(h->store,MD_STORE_MANAGEMENT);
    CHECK(f);
    NostrFilter *filter = nostr_filter_new();
    CHECK(filter && nostr_filter_deserialize_compact(filter,f,NULL));
    free(f);
    NostrFilters *filters = nostr_filters_new();
    CHECK(filters);
    nostr_filters_add(filters,filter);
    nostr_filter_free(filter);
    const char *urls[] = {url};
    NostrPoolSubscription *sub = nostr_simple_pool_subscription_open(pool,urls,1,filters,true,event,state,h);
    nostr_filters_free(filters);
    CHECK(sub);
    uint64_t deadline = md_monotonic_ns() + 8000000000ull;
    pthread_mutex_lock(&h->mu);
    while (!h->eose && !h->closed) CHECK(!md_cond_timedwait_until(&h->changed,&h->mu,deadline));
    CHECK(h->eose && !h->closed);
    pthread_mutex_unlock(&h->mu);
    return sub;
}
static int publish(MdAccountBinding *binding, const char *url, const char *wire) {
    const char *relays[] = {url};
    MdNostrConfig cfg = {.owner=binding,.relay_urls=relays,.relay_count=1};
    MdNostr *n = md_nostr_create(&cfg,NULL);
    CHECK(n);
    int rc = md_nostr_publish_signed_json(n,wire);
    md_nostr_destroy(n);
    return rc;
}
static void journal_path(char *path, size_t capacity, const char *root, const char *d) {
    char name[64];
    CHECK(strlen(d) < sizeof(name));
    strcpy(name,d);
    for (char *p = name; *p; ++p) if (*p == ':') *p = '_';
    size_t len = strlen(root);
    const char *sep = len && (root[len - 1] == '/' || root[len - 1] == '\\') ? "" : "/";
    int n = snprintf(path,capacity,"%s%s%s/%s",root,sep,PK,name);
    CHECK(n > 0 && (size_t)n < capacity);
}
static void check_keep_remote(MdManagedStore *s, MdSigner *client, const char *storage,
    const char *const *targets, int64_t now) {
    static const char *const docs[] = {
        "metadesk-tag:86b09912-5e73-4bb0-a708-30c770882acf",
        "metadesk-tag:86b09912-5e73-4bb0-a708-30c770882ac0"
    };
    for (size_t i = 0; i < sizeof(docs)/sizeof(docs[0]); ++i) {
        const char *d = docs[i];
        int kind = NOSTR_KIND_CATEGORIZED_PEOPLE_LIST;
        const char *remote_plain = "[[\"p\",\"" OTHER "\"]]";
        char *a = make(client,PK,d,kind,"[]",now-100);
        char *b = make(client,PK,d,kind,remote_plain,now-50);
        char *a_id = event_id(a), *b_id = event_id(b);
        CHECK(md_managed_store_receive(s,d,a,now) == MD_STORE_OK);
        CHECK(md_managed_store_backfill(s,d,true) == MD_STORE_OK);
        CHECK(md_managed_store_stage(s,d,a_id,"[[\"p\",\"" HOST "\"]]",targets,2,now) == MD_STORE_PENDING);
        char *discarded = NULL;
        if (i == 0) {
            /* A crash after handoff but before completion must not make the
             * draft look known-unsent. No explicit completion is recorded. */
            CHECK(md_managed_store_outgoing(s,d,targets[0],&discarded) == MD_STORE_PENDING);
        } else {
            /* Independently prove that a wire echo, with all targets still
             * WAITING, counts as evidence that the draft escaped. */
            char path[1024]; journal_path(path,sizeof(path),storage,d);
            json_t *j = json_load_file(path,0,NULL); CHECK(j);
            const char *stored = json_string_value(json_object_get(j,"draft")); CHECK(stored);
            discarded = strdup(stored);
            json_decref(j);
        }
        CHECK(discarded);
        CHECK(md_managed_store_receive(s,d,b,now) == MD_STORE_CONFLICT);
        if (i == 1) CHECK(md_managed_store_receive(s,d,discarded,now) == MD_STORE_UNCHANGED);
        CHECK(md_managed_store_keep_remote(s,d,a_id,now) == MD_STORE_CONFLICT);
        CHECK(md_managed_store_keep_remote(s,d,b_id,now) == MD_STORE_PENDING);
        CHECK(md_managed_store_receive(s,d,discarded,now) == MD_STORE_UNCHANGED);
        char *fixed = NULL, *plain = NULL, *pending = NULL, rev[65];
        CHECK(md_managed_store_read(s,d,rev,&plain,&pending) == MD_STORE_PENDING);
        CHECK(!strcmp(rev,b_id) && !strcmp(plain,remote_plain) && !strcmp(pending,remote_plain));
        CHECK(md_managed_store_outgoing(s,d,targets[0],&fixed) == MD_STORE_PENDING);
        NostrEvent *e = nostr_event_new(); CHECK(e);
        CHECK(nostr_event_deserialize_signed(e,fixed,NULL) == NOSTR_EVENT_VALIDATION_OK);
        CHECK(nostr_event_get_created_at(e) == now+1);
        char *fixed_id = event_id(fixed);
        CHECK(md_managed_store_record_result(s,d,targets[0],fixed_id,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_PARTIAL);
        CHECK(md_managed_store_record_result(s,d,targets[1],fixed_id,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_OK);
        CHECK(md_managed_store_receive(s,d,discarded,now) == MD_STORE_UNCHANGED);
        CHECK(md_managed_store_read(s,d,rev,NULL,NULL) == MD_STORE_OK && !strcmp(rev,fixed_id));
        nostr_event_free(e); free(fixed_id); free(pending); free(plain); free(fixed);
        free(discarded); free(a); free(b); free(a_id); free(b_id);
    }
}
#define ADMISSION_TAG "metadesk-tag:86b09912-5e73-4bb0-a708-30c770882ac1"

static void overwrite_file(const char *path, const char *text) {
    FILE *f = fopen(path,"w"); CHECK(f);
    CHECK(fputs(text,f) >= 0 && !fclose(f));
}
static void check_admission(MdManagedStore *s, MdSigner *client, const char *storage,
    const char *const *targets, int64_t now) {
    const int kind = NOSTR_KIND_CATEGORIZED_PEOPLE_LIST;
    const char *d = ADMISSION_TAG;
    char *valid = make(client,PK,d,kind,"[]",now);
    char *valid_id = event_id(valid);
    CHECK(md_managed_store_receive(s,d,valid,now) == MD_STORE_OK);
    char rev[65];
    static const struct { const char *name, *field; int change; } cases[] = {
        {"bad signature","sig",0}, {"mismatched id","id",0}, {"wrong kind","kind",1},
        {"wrong d-tag","tags",2}, {"second d-tag","tags",3}, {"extra public tag","tags",4},
        {"non-NIP-44 content","content",5}, {"duplicate keys","id",6}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        json_t *j = json_loads(valid,JSON_REJECT_DUPLICATES,NULL); CHECK(j);
        char *wire = NULL;
        switch (cases[i].change) {
        case 0: {
            char *tampered = strdup(json_string_value(json_object_get(j,cases[i].field))); CHECK(tampered);
            tampered[0] = tampered[0] == 'a' ? 'b' : 'a';
            CHECK(!json_object_set_new(j,cases[i].field,json_string(tampered))); free(tampered);
            break;
        }
        case 1: CHECK(!json_object_set_new(j,"kind",json_integer(NOSTR_KIND_APPLICATION_SPECIFIC_DATA))); break;
        case 2: CHECK(!json_object_set_new(j,"tags",json_pack("[[s,s]]","d",TAG))); break;
        case 3: CHECK(!json_array_append_new(json_object_get(j,"tags"),json_pack("[s,s]","d",TAG))); break;
        case 4: CHECK(!json_array_append_new(json_object_get(j,"tags"),json_pack("[s,s]","p",HOST))); break;
        case 5: CHECK(!json_object_set_new(j,"content",json_string("[]"))); break;
        case 6: {
            size_t n = strlen(valid);
            wire = malloc(n+80); CHECK(wire);
            snprintf(wire,n+80,"%.*s,\"id\":\"%s\"}",(int)(n-1),valid,valid_id);
            break;
        }
        }
        if (cases[i].change > 0 && cases[i].change < 6) {
            CHECK(!json_object_del(j,"id") && !json_object_del(j,"sig"));
            CHECK(!json_object_set_new(j,"created_at",json_integer(now+1)));
            char *input = json_dumps(j,JSON_COMPACT); CHECK(input);
            CHECK(!md_signer_sign_event(client,input,&wire)); free(input);
        }
        if (!wire) wire = json_dumps(j,JSON_COMPACT);
        json_decref(j); CHECK(wire);
        json_t *parsed = json_loads(wire,0,NULL); CHECK(parsed); json_decref(parsed);
        if (cases[i].change != 6) {
            NostrEvent *e = nostr_event_new(); CHECK(e);
            CHECK(nostr_event_deserialize_signed(e,wire,NULL) == NOSTR_EVENT_VALIDATION_OK);
            char canonical[65];
            NostrEventValidationStatus status = nostr_event_validate(e,canonical);
            CHECK((status == NOSTR_EVENT_VALIDATION_OK) == (cases[i].change != 0));
            nostr_event_free(e);
        }
        if (md_managed_store_receive(s,d,wire,now) != MD_STORE_INVALID) {
            fprintf(stderr,"unexpected admission: %s\n",cases[i].name); abort();
        }
        CHECK(md_managed_store_read(s,d,rev,NULL,NULL) == MD_STORE_OK && !strcmp(rev,valid_id));
        free(wire);
    }
    /* Content over the document cap but wire within the envelope allowance.
     * Sign locally with the throwaway fixture key: the NIP-5F test RPC has a
     * smaller wire limit and would reject the case before admission. */
    NostrEvent *huge = nostr_event_new(); CHECK(huge);
    CHECK(nostr_event_deserialize_signed(huge,valid,NULL) == NOSTR_EVENT_VALIDATION_OK);
    nostr_event_set_created_at(huge,now+1);
    char *padding = malloc(MD_STORE_MAX_CONTENT+5); CHECK(padding);
    memset(padding,'g',MD_STORE_MAX_CONTENT+4); padding[0] = 'A'; padding[MD_STORE_MAX_CONTENT+4] = 0;
    nostr_event_set_content(huge,padding); free(padding);
    CHECK(!nostr_event_sign(huge,"0000000000000000000000000000000000000000000000000000000000000002"));
    char *wire = nostr_event_serialize_compact(huge); CHECK(wire);
    CHECK(strlen(wire) <= MD_STORE_MAX_CONTENT+1024);
    json_t *parsed = json_loads(wire,JSON_REJECT_DUPLICATES,NULL); CHECK(parsed); json_decref(parsed);
    char canonical[65]; CHECK(nostr_event_validate(huge,canonical) == NOSTR_EVENT_VALIDATION_OK);
    CHECK(md_managed_store_receive(s,d,wire,now) == MD_STORE_LIMIT);
    CHECK(md_managed_store_read(s,d,rev,NULL,NULL) == MD_STORE_OK && !strcmp(rev,valid_id));
    free(wire); nostr_event_free(huge);

    char path[1024]; journal_path(path,sizeof(path),storage,d);
    CHECK(md_managed_store_backfill(s,d,true) == MD_STORE_OK);
#ifdef _WIN32
    CHECK(md_test_win_acl(path,false));
    CHECK(md_managed_store_read(s,d,rev,NULL,NULL) == MD_STORE_IO && !*rev);
    CHECK(md_managed_store_receive(s,d,valid,now) == MD_STORE_IO);
    CHECK(md_test_win_acl(path,true));
    CHECK(md_managed_store_read(s,d,rev,NULL,NULL) == MD_STORE_OK && !strcmp(rev,valid_id));
#endif
    const char *const damaged[] = {"{", "{}", "{\"event\":17}"};
    for (size_t i = 0; i < sizeof(damaged)/sizeof(damaged[0]); ++i) {
        overwrite_file(path,damaged[i]);
        CHECK(md_managed_store_read(s,d,rev,NULL,NULL) == MD_STORE_IO && !*rev);
        CHECK(md_managed_store_receive(s,d,valid,now) == MD_STORE_IO);
        CHECK(md_managed_store_stage(s,d,"","[]",targets,1,now) == MD_STORE_IO);
    }
    free(valid); free(valid_id);
}

static void check_relay_outcomes(MdManagedStore *s, MdSigner *client, const char *const *targets, int64_t now) {
    const char *d = "metadesk-tag:86b09912-5e73-4bb0-a708-30c770882ac2";
    char *wire = NULL, rev[65];
    CHECK(md_managed_store_backfill(s,d,true) == MD_STORE_OK);
    CHECK(md_managed_store_stage(s,d,"","[]",targets,2,now) == MD_STORE_PENDING);
    CHECK(md_managed_store_outgoing(s,d,targets[0],&wire) == MD_STORE_PENDING);
    char *draft_id = event_id(wire);
    const MdStoreRelayResult failures[] = {
        MD_STORE_RELAY_AUTH_REQUIRED, MD_STORE_RELAY_CLOSED, MD_STORE_RELAY_UNKNOWN
    };
    for (size_t i = 0; i < sizeof(failures)/sizeof(failures[0]); ++i) {
        CHECK(md_managed_store_record_result(s,d,targets[0],draft_id,failures[i]) == MD_STORE_PENDING);
        MdStoreRelayResult outcome;
        CHECK(md_managed_store_relay_result(s,d,targets[0],&outcome) == MD_STORE_OK && outcome == failures[i]);
        CHECK(md_managed_store_read(s,d,rev,NULL,NULL) == MD_STORE_PENDING && !*rev);
    }
    CHECK(md_managed_store_record_result(s,d,targets[0],draft_id,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_PARTIAL);
    CHECK(md_managed_store_record_result(s,d,targets[0],draft_id,MD_STORE_RELAY_REJECTED) == MD_STORE_PARTIAL);
    MdStoreRelayResult outcome;
    CHECK(md_managed_store_relay_result(s,d,targets[0],&outcome) == MD_STORE_OK && outcome == MD_STORE_RELAY_ACKNOWLEDGED);
    CHECK(md_managed_store_read(s,d,rev,NULL,NULL) == MD_STORE_PARTIAL && !strcmp(rev,draft_id));
    char *remote = make(client,PK,d,NOSTR_KIND_CATEGORIZED_PEOPLE_LIST,"[[\"p\",\"" OTHER "\"]]",now+1);
    CHECK(md_managed_store_receive(s,d,remote,now) == MD_STORE_CONFLICT);
    CHECK(md_managed_store_receive(s,d,wire,now) == MD_STORE_UNCHANGED);
    CHECK(md_managed_store_record_result(s,d,targets[1],draft_id,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_CONFLICT);
    char *remote_id = event_id(remote), *plain = NULL, *pending = NULL;
    CHECK(md_managed_store_read(s,d,rev,&plain,&pending) == MD_STORE_CONFLICT);
    CHECK(!strcmp(rev,remote_id) && strstr(plain,OTHER) && !strcmp(pending,"[]"));
    free(plain); free(pending); free(remote); free(remote_id); free(draft_id); free(wire);
}

static void check_normalized_receipts(MdManagedStore *s, int64_t now) {
    const char *d = "metadesk-tag:86b09912-5e73-4bb0-a708-30c770882ac3";
    const char *alias = "WS://LOCALHOST:1234/Path/";
    const char *normalized = "ws://localhost:1234/Path";
    const char *const targets[] = {alias, normalized};
    CHECK(md_managed_store_backfill(s,d,true) == MD_STORE_OK);
    CHECK(md_managed_store_stage(s,d,"","[]",targets,2,now) == MD_STORE_INVALID);
    CHECK(md_managed_store_stage(s,d,"","[]",targets,1,now) == MD_STORE_PENDING);
    char *wire = NULL;
    CHECK(md_managed_store_outgoing(s,d,"ws://localhost:1234/path",&wire) == MD_STORE_INVALID && !wire);
    CHECK(md_managed_store_outgoing(s,d,normalized,&wire) == MD_STORE_PENDING);
    char *event = event_id(wire);
    MdStoreRelayResult observed;
    CHECK(md_managed_store_relay_result(s,d,alias,&observed) == MD_STORE_OK && observed == MD_STORE_RELAY_UNKNOWN);
    CHECK(md_managed_store_record_result(s,d,normalized,event,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_OK);
    free(event); free(wire);
}

static void check_exchange(void) {
    char *text = NULL, *candidates = NULL;
    const char *input = "{\"schema\":\"metadesk-hosts/1\",\"hosts\":[{\"pubkey\":\"" HOST
        "\",\"label\":\"l\",\"notes\":\"n\",\"credential\":\"secret\",\"grant\":\"input\"}]}";
    CHECK(md_managed_store_import(input,&candidates) == MD_STORE_OK);
    CHECK(candidates && !strstr(candidates,"secret") && !strstr(candidates,"grant"));
    CHECK(md_managed_store_export(candidates,&text) == MD_STORE_OK);
    CHECK(text && strstr(text,"metadesk-hosts/1") && !strstr(text,"secret"));
    free(text); free(candidates);
    CHECK(md_managed_store_import("{\"schema\":\"metadesk-hosts/2\",\"hosts\":[]}",&candidates) == MD_STORE_READ_ONLY);
    CHECK(!candidates);
    CHECK(md_managed_store_export("[{\"pubkey\":\"" HOST "\",\"relay_hints\":[\"wss://user:pass@example.org\"]}]",&text) == MD_STORE_INVALID);
}
static void check_offline_quick_hosts(MdManagedStore *s, MdAccountBinding *binding,
        MdSigner *signer, const char *root, const char *const *targets, int64_t now) {
    const char *const keys[] = {HOST, OTHER, HOST_PK, PK};
    const char *const documents[] = {MD_STORE_HOSTS, MD_STORE_MANAGEMENT, MD_STORE_FAVORITES};
    const char *const payloads[] = {
        "[[\"p\",\"" HOST "\"],[\"p\",\"" OTHER "\"],[\"p\",\"" HOST_PK "\"]]",
        "{\"schema_version\":1,\"hosts\":{\"" HOST "\":{\"label\":\"One\",\"notes\":\"" SECRET "\"},"
        "\"" OTHER "\":{\"nip05\":\"desk@example.org\"},\"" PK "\":{}}}",
        "[[\"p\",\"" PK "\"],[\"p\",\"" HOST_PK "\"],[\"p\",\"" HOST "\"]]",
    };
    for (size_t i = 0; i < 3; ++i) {
        char *event = make(signer, PK, documents[i], i == 1 ? NOSTR_KIND_APPLICATION_SPECIFIC_DATA :
            NOSTR_KIND_CATEGORIZED_PEOPLE_LIST, payloads[i], now);
        CHECK(md_managed_store_receive(s, documents[i], event, now) == MD_STORE_OK);
        free(event);
    }
    for (size_t i = 0; i < 4; ++i)
        CHECK(md_managed_store_record(s, keys[i], now, MD_RECENT_CONNECTED) == MD_STORE_OK);
    CHECK(md_managed_store_record(s, OTHER, now, MD_RECENT_CONNECTED) == MD_STORE_OK);
    /* No live controller/REQ or unlocked writer is needed to read the cache. */
    json_t *rows = md_managed_store_quick_hosts(binding, root);
    CHECK(json_array_size(rows) == 2);
    for (size_t i = 0; i < 2; ++i) {
        json_t *row = json_array_get(rows, i);
        CHECK(!strcmp(json_string_value(json_object_get(row, "pubkey")), keys[i]));
        CHECK(json_is_true(json_object_get(row, "favorite")) == (i == 0));
        CHECK(!json_object_get(row, "notes"));
    }
    CHECK(!strcmp(json_string_value(json_object_get(json_array_get(rows, 1), "nip05")), "desk@example.org"));
    md_managed_store_discard_plaintext(rows);

    /* No EVENT handoff or positive OK: each draft can only subtract pins.
     * Keep Remote restores eligibility when an unexposed draft is discarded. */
    const char *const drafts[] = {
        "[[\"p\",\"" OTHER "\"]]",
        "{\"schema_version\":1,\"hosts\":{\"" OTHER "\":{\"nip05\":\"desk@example.org\"},\"" PK "\":{\"label\":\"New pin\"}}}",
        "[]",
    };
    for (size_t i = 0; i < 3; ++i) {
        char base[65];
        CHECK(md_managed_store_read(s, documents[i], base, NULL, NULL) == MD_STORE_OK);
        CHECK(md_managed_store_backfill(s, documents[i], true) == MD_STORE_OK);
        CHECK(md_managed_store_stage(s, documents[i], base, drafts[i], targets, 1, now + 1) == MD_STORE_PENDING);
        rows = md_managed_store_quick_hosts(binding, root);
        CHECK(json_is_array(rows) && json_array_size(rows) == (i == 2 ? 0 : 1));
        if (i != 2) CHECK(!strcmp(json_string_value(json_object_get(json_array_get(rows, 0), "pubkey")), OTHER));
        md_managed_store_discard_plaintext(rows);
        /* Keep Remote applies to conflicts. Simulate a same-content winner
         * from another device, then discard the local draft. */
        char *remote = make(signer, PK, documents[i], i == 1 ? NOSTR_KIND_APPLICATION_SPECIFIC_DATA :
            NOSTR_KIND_CATEGORIZED_PEOPLE_LIST, payloads[i], now + 2);
        CHECK(md_managed_store_receive(s, documents[i], remote, now + 2) == MD_STORE_CONFLICT);
        free(remote);
        CHECK(md_managed_store_read(s, documents[i], base, NULL, NULL) == MD_STORE_CONFLICT);
        CHECK(md_managed_store_keep_remote(s, documents[i], base, now + 2) == MD_STORE_OK);
        rows = md_managed_store_quick_hosts(binding, root);
        CHECK(json_array_size(rows) == 2);
        md_managed_store_discard_plaintext(rows);
    }

    /* After a manager deletes a pin, orphan favorites/recents cannot restore it. */
    char *deleted = make(signer, PK, MD_STORE_HOSTS, NOSTR_KIND_CATEGORIZED_PEOPLE_LIST,
        "[[\"p\",\"" OTHER "\"]]", now + 3);
#ifdef _WIN32
    /* Hold the old journal open across the commit, like an overlapping
     * quick-hosts reader. No race timing is needed to exercise replacement. */
    char journal[1024];
    journal_path(journal, sizeof(journal), root, MD_STORE_HOSTS);
    char *name = strrchr(journal, '/');
    CHECK(name);
    *name++ = 0;
    MdWinDirectory *dir = md_win_directory(journal, false);
    CHECK(dir);
    HANDLE reader = md_win_file(dir, name, GENERIC_READ, OPEN_EXISTING,
                                FILE_SHARE_READ | FILE_SHARE_DELETE);
    CHECK(reader != INVALID_HANDLE_VALUE);
    unsigned char *before = NULL, *after = NULL;
    size_t before_size = 0, after_size = 0;
    CHECK(md_win_read(reader, &before, &before_size, 1024 * 1024));
    CHECK(SetFilePointerEx(reader, (LARGE_INTEGER){0}, NULL, FILE_BEGIN));
#endif
    CHECK(md_managed_store_receive(s, MD_STORE_HOSTS, deleted, now + 3) == MD_STORE_OK);
    free(deleted);
#ifdef _WIN32
    CHECK(md_win_read(reader, &after, &after_size, 1024 * 1024));
    CHECK(before_size == after_size && !memcmp(before, after, before_size));
    free(before);
    free(after);
#endif
    rows = md_managed_store_quick_hosts(binding, root);
    CHECK(json_array_size(rows) == 1);
    CHECK(!strcmp(json_string_value(json_object_get(json_array_get(rows, 0), "pubkey")), OTHER));
    md_managed_store_discard_plaintext(rows);
#ifdef _WIN32
    CloseHandle(reader);
    md_win_directory_close(dir);
    puts("PASS: held Windows snapshot survives concurrent journal replacement");
#endif
}

int main(int argc, char **argv) {
#ifdef _WIN32
    CHECK(argc == 6);
    /* Windows has no NIP-5F backend in the producer. Use real in-process
     * crypto with the same fixture keys, without persisting credentials. */
    MdSignerType backend = MD_SIGNER_DIRECT_KEY;
    MdSigner *client = md_signer_create_direct("0000000000000000000000000000000000000000000000000000000000000002");
    MdSigner *other = md_signer_create_direct("0000000000000000000000000000000000000000000000000000000000000001");
    MdWinDirectory *storage = md_win_directory(argv[2], true);
    CHECK(storage);
    md_win_directory_close(storage);
#else
    CHECK(argc == 8);
    MdSignerType backend = MD_SIGNER_NIP5F;
    MdSigner *client = md_signer_create_nip5f(argv[3]);
    MdSigner *other = md_signer_create_nip5f(argv[4]);
#endif
    const char *targets[] = {argv[argc - 3],argv[argc - 2]};
    int64_t now = (int64_t)time(NULL);
    CHECK(client && other);
    MdAccounts *accounts = NULL;
    CHECK(!md_accounts_open(argv[1],NULL,&accounts));
    CHECK(!md_accounts_add(accounts,backend,PK,"client"));
    CHECK(!md_accounts_add(accounts,backend,HOST_PK,"other"));
    CHECK(!md_accounts_bind_role(accounts,MD_ACCOUNT_CLIENT,backend,PK,client));
    CHECK(!md_accounts_bind_role(accounts,MD_ACCOUNT_HOST,backend,HOST_PK,other));
    MdAccountBinding *host = md_accounts_acquire_role(accounts,MD_ACCOUNT_HOST);
    CHECK(host && !md_managed_store_open(host,argv[2]));
    CHECK(!md_managed_store_quick_hosts(host, argv[2]));
    md_account_binding_release(host);
    MdAccountBinding *binding = md_accounts_acquire_role(accounts,MD_ACCOUNT_CLIENT);
    MdManagedStore *s = md_managed_store_open(binding,argv[2]);
    CHECK(s && !md_managed_store_open(binding,argv[2]));
    bool crash = !strcmp(argv[argc - 1],"crash");
    char *wire = NULL, *plain = NULL, *pending = NULL, revision[65];
    CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,"",V1,targets,2,now) == MD_STORE_NOT_READY);
    if (crash) {
        CHECK(md_managed_store_backfill(s,MD_STORE_MANAGEMENT,true) == MD_STORE_OK);
        CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,"",V1,targets,2,now) == MD_STORE_PENDING);
        _exit(42); /* No teardown after durable write, before any publication. */
    }
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,&pending) == MD_STORE_PENDING);
    CHECK(!*revision && !plain && strstr(pending,SECRET)); free(pending);

    History h = {.store=s,.mu=PTHREAD_MUTEX_INITIALIZER,.now=now};
    CHECK(!md_cond_init_monotonic(&h.changed));
    NostrSimplePool *pool = nostr_simple_pool_new(); CHECK(pool);
    nostr_simple_pool_start(pool);
    NostrPoolSubscription *sub = subscribe(pool,targets[0],&h);
    pthread_mutex_lock(&h.mu);
    CHECK(md_managed_store_outgoing(s,MD_STORE_MANAGEMENT,targets[1],&wire) == MD_STORE_PENDING);
    char *first_id = event_id(wire);
    CHECK(md_managed_store_record_result(s,MD_STORE_MANAGEMENT,targets[0],HOST,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_UNCHANGED);
    pthread_mutex_unlock(&h.mu);
    CHECK(publish(binding,targets[1],wire) != 0);
    pthread_mutex_lock(&h.mu);
    CHECK(md_managed_store_record_result(s,MD_STORE_MANAGEMENT,targets[1],first_id,MD_STORE_RELAY_REJECTED) == MD_STORE_PENDING);
    CHECK(md_managed_store_receive(s,MD_STORE_MANAGEMENT,wire,now) == MD_STORE_UNCHANGED);
    char *retry = NULL;
    CHECK(md_managed_store_outgoing(s,MD_STORE_MANAGEMENT,targets[1],&retry) == MD_STORE_PENDING && !strcmp(wire,retry));
    free(retry);
    pthread_mutex_unlock(&h.mu);
    CHECK(!publish(binding,targets[0],wire));
    pthread_mutex_lock(&h.mu);
    CHECK(md_managed_store_record_result(s,MD_STORE_MANAGEMENT,targets[0],first_id,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_PARTIAL);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,NULL) == MD_STORE_PARTIAL);
    CHECK(!strcmp(revision,first_id) && strstr(plain,SECRET)); free(plain);
    pthread_mutex_unlock(&h.mu);
    nostr_pool_subscription_free(sub);
    md_managed_store_close(s);
    h.store = s = md_managed_store_open(binding,argv[2]);
    CHECK(s && md_managed_store_outgoing(s,MD_STORE_MANAGEMENT,targets[1],&retry) == MD_STORE_NOT_READY);
    h.eose = false;
    h.received = 0;
    sub = subscribe(pool,targets[0],&h);
    pthread_mutex_lock(&h.mu);
    CHECK(h.received == 1);
    MdStoreRelayResult receipt;
    CHECK(md_managed_store_relay_result(s,MD_STORE_MANAGEMENT,targets[0],&receipt) == MD_STORE_OK && receipt == MD_STORE_RELAY_ACKNOWLEDGED);
    CHECK(md_managed_store_relay_result(s,MD_STORE_MANAGEMENT,targets[1],&receipt) == MD_STORE_OK && receipt == MD_STORE_RELAY_REJECTED);
    CHECK(md_managed_store_outgoing(s,MD_STORE_MANAGEMENT,targets[0],&retry) == MD_STORE_UNCHANGED);
    CHECK(md_managed_store_outgoing(s,MD_STORE_MANAGEMENT,targets[1],&retry) == MD_STORE_PARTIAL && !strcmp(wire,retry));
    pthread_mutex_unlock(&h.mu);
    CHECK(!publish(binding,targets[1],retry));
    pthread_mutex_lock(&h.mu);
    CHECK(md_managed_store_record_result(s,MD_STORE_MANAGEMENT,targets[1],first_id,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_OK);
    pthread_mutex_unlock(&h.mu);
    free(wire); free(retry);
    nostr_pool_subscription_free(sub);
    nostr_simple_pool_free(pool);
    pthread_cond_destroy(&h.changed);
    pthread_mutex_destroy(&h.mu);

    const char *edited = "{\"schema_version\":1,\"hosts\":{\"" HOST "\":{\"label\":\"edited\"}}}";
    CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,"",edited,targets,2,now) == MD_STORE_CONFLICT);
    CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,first_id,edited,targets,2,now) == MD_STORE_PENDING);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,&pending) == MD_STORE_PENDING);
    CHECK(strstr(plain,SECRET) && strstr(pending,"future_host") && strstr(pending,"future") && strstr(pending,"quality"));
    free(plain); free(pending); free(first_id);
    char *review_base = make(client,PK,MD_STORE_MANAGEMENT,NOSTR_KIND_APPLICATION_SPECIFIC_DATA,
        "{\"schema_version\":1,\"remote_change\":true}",now+5);
    CHECK(md_managed_store_receive(s,MD_STORE_MANAGEMENT,review_base,now) == MD_STORE_CONFLICT);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,&pending) == MD_STORE_CONFLICT);
    CHECK(strstr(plain,"remote_change") && strstr(pending,"edited"));
    free(plain); free(pending); free(review_base);
    CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,revision,edited,targets,2,now) == MD_STORE_CONFLICT);
    CHECK(md_managed_store_reapply(s,MD_STORE_MANAGEMENT,revision,"[]",targets,2,now) == MD_STORE_INVALID);
    CHECK(md_managed_store_reapply(s,MD_STORE_MANAGEMENT,revision,edited,targets,2,now) == MD_STORE_PENDING);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,NULL,&pending) == MD_STORE_PENDING);
    CHECK(strstr(pending,"remote_change") && strstr(pending,"edited")); free(pending);
    const char *future = "{\"schema_version\":2,\"hosts\":\"different layout\"}";
    char *remote = make(client,PK,MD_STORE_MANAGEMENT,NOSTR_KIND_APPLICATION_SPECIFIC_DATA,future,now+10);
    CHECK(md_managed_store_receive(s,MD_STORE_MANAGEMENT,remote,now) == MD_STORE_CONFLICT);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,&pending) == MD_STORE_READ_ONLY);
    CHECK(strstr(plain,"different layout") && strstr(pending,"edited"));
    CHECK(md_managed_store_replace_invalid(s,MD_STORE_MANAGEMENT,revision,V1,targets,1,now) == MD_STORE_READ_ONLY);
    free(plain); free(pending); free(remote);
    CHECK(md_managed_store_outgoing(s,MD_STORE_MANAGEMENT,targets[0],&wire) == MD_STORE_CONFLICT && !wire);
    CHECK(md_managed_store_keep_remote(s,MD_STORE_MANAGEMENT,revision,now) == MD_STORE_OK);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,NULL,NULL) == MD_STORE_READ_ONLY);
    CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,revision,V1,targets,1,now) == MD_STORE_READ_ONLY);
    char long_note[2050]; memset(long_note,'x',sizeof(long_note)-1); long_note[sizeof(long_note)-1] = 0;
    json_t *bad_doc = json_pack("{s:i,s:{s:{s:s}}}","schema_version",1,"hosts",HOST,"notes",long_note);
    CHECK(bad_doc);
    char *invalid_plain = json_dumps(bad_doc,JSON_COMPACT); CHECK(invalid_plain);
    json_decref(bad_doc);
    char *invalid = make(client,PK,MD_STORE_MANAGEMENT,NOSTR_KIND_APPLICATION_SPECIFIC_DATA,invalid_plain,now+11);
    char *invalid_id = event_id(invalid);
    CHECK(md_managed_store_receive(s,MD_STORE_MANAGEMENT,invalid,now) == MD_STORE_OK);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,NULL) == MD_STORE_READ_ONLY);
    CHECK(!plain && !strcmp(revision,invalid_id));
    CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,revision,V1,targets,1,now) == MD_STORE_READ_ONLY);
    CHECK(md_managed_store_replace_invalid(s,MD_STORE_MANAGEMENT,HOST,V1,targets,1,now) == MD_STORE_CONFLICT);
    CHECK(md_managed_store_replace_invalid(s,MD_STORE_MANAGEMENT,revision,V1,targets,1,now) == MD_STORE_PENDING);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,&pending) == MD_STORE_READ_ONLY);
    CHECK(!plain && !strcmp(revision,invalid_id) && strstr(pending,SECRET));
    free(pending); free(invalid_id); free(invalid); free(invalid_plain);
    CHECK(md_managed_store_outgoing(s,MD_STORE_MANAGEMENT,targets[0],&wire) == MD_STORE_PENDING);
    char *repaired_id = event_id(wire);
    CHECK(md_managed_store_record_result(s,MD_STORE_MANAGEMENT,targets[0],repaired_id,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_OK);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,NULL) == MD_STORE_OK);
    CHECK(!strcmp(revision,repaired_id) && strstr(plain,SECRET)); free(plain); free(repaired_id); free(wire);
    const char *clear = "{\"schema_version\":1,\"hosts\":{\"" HOST "\":{\"quality\":null}}}";
    CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,revision,clear,targets,1,now) == MD_STORE_PENDING);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,&pending) == MD_STORE_PENDING);
    CHECK(strstr(plain,"quality") && !strstr(pending,"quality") && strstr(pending,"future_host"));
    free(plain); free(pending);
    CHECK(md_managed_store_outgoing(s,MD_STORE_MANAGEMENT,targets[0],&wire) == MD_STORE_PENDING);
    char *clear_id = event_id(wire);
    CHECK(md_managed_store_record_result(s,MD_STORE_MANAGEMENT,targets[0],clear_id,MD_STORE_RELAY_ACKNOWLEDGED) == MD_STORE_OK);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,NULL) == MD_STORE_OK);
    CHECK(!strcmp(revision,clear_id) && !strstr(plain,"quality")); free(plain); free(clear_id); free(wire);

    /* A structurally valid encrypted envelope with a bad MAC is a decrypt
     * failure, not a schema error eligible for explicit recovery. */
    NostrEvent *bad_mac = md_private_document_build(client,NULL,PK,NOSTR_KIND_APPLICATION_SPECIFIC_DATA,
        MD_STORE_MANAGEMENT,V1,now+14); CHECK(bad_mac);
    char *bad_cipher = strdup(nostr_event_get_content(bad_mac)); CHECK(bad_cipher);
    size_t n_cipher = strlen(bad_cipher); CHECK(n_cipher > 10);
    bad_cipher[n_cipher-8] = bad_cipher[n_cipher-8] == 'A' ? 'B' : 'A';
    nostr_event_set_content(bad_mac,bad_cipher); free(bad_cipher);
    CHECK(!md_nostr_sign_event_json(client,bad_mac,&wire)); nostr_event_free(bad_mac);
    char *bad_mac_id = event_id(wire);
    CHECK(md_managed_store_receive(s,MD_STORE_MANAGEMENT,wire,now) == MD_STORE_OK);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,NULL) == MD_STORE_SIGNER);
    CHECK(!*revision && !plain);
    CHECK(md_managed_store_replace_invalid(s,MD_STORE_MANAGEMENT,bad_mac_id,V1,targets,1,now) == MD_STORE_SIGNER);
    free(bad_mac_id); free(wire);

    const char *p1 = "[[\"p\",\"" HOST "\"],[\"extension\",\"retain\"]]";
    const char *p2 = "[[\"p\",\"" OTHER "\"]]";
    int kind = NOSTR_KIND_CATEGORIZED_PEOPLE_LIST;
    char *a = make(client,PK,MD_STORE_HOSTS,kind,p1,now+2);
    char *b = make(client,PK,MD_STORE_HOSTS,kind,p2,now+2);
    char *aid = event_id(a), *bid = event_id(b);
    bool a_wins = strcmp(aid,bid) < 0;
    CHECK(md_managed_store_receive(s,MD_STORE_HOSTS,a_wins ? b : a,now) == MD_STORE_OK);
    CHECK(md_managed_store_receive(s,MD_STORE_HOSTS,a_wins ? a : b,now) == MD_STORE_OK);
    CHECK(md_managed_store_receive(s,MD_STORE_HOSTS,a_wins ? b : a,now) == MD_STORE_UNCHANGED);
    CHECK(md_managed_store_receive(s,MD_STORE_HOSTS,a_wins ? a : b,now) == MD_STORE_UNCHANGED);
    char *old = make(client,PK,MD_STORE_HOSTS,kind,p1,now+1);
    CHECK(md_managed_store_receive(s,MD_STORE_HOSTS,old,now) == MD_STORE_UNCHANGED); free(old);
    CHECK(md_managed_store_read(s,MD_STORE_HOSTS,revision,&plain,NULL) == MD_STORE_OK);
    CHECK(!strcmp(revision,a_wins ? aid : bid) && strstr(plain,a_wins ? HOST : OTHER));
    free(plain); free(a); free(b); free(aid); free(bid);
    a = make(other,HOST_PK,MD_STORE_HOSTS,kind,p1,now+3);
    CHECK(md_managed_store_receive(s,MD_STORE_HOSTS,a,now) == MD_STORE_INVALID); free(a);
    a = make(client,PK,TAG,kind,p1,now+301);
    CHECK(md_managed_store_receive(s,TAG,a,now) == MD_STORE_CLOCK);
    CHECK(md_managed_store_receive(s,TAG,a,now+1) == MD_STORE_OK); free(a);
    CHECK(md_managed_store_backfill(s,TAG,true) == MD_STORE_OK);
    CHECK(md_managed_store_read(s,TAG,revision,NULL,NULL) == MD_STORE_OK);
    CHECK(md_managed_store_stage(s,TAG,revision,p2,targets,1,now+2) == MD_STORE_PENDING);
    CHECK(md_managed_store_read(s,TAG,revision,NULL,&pending) == MD_STORE_PENDING);
    CHECK(strstr(pending,"retain") && !strstr(pending,HOST)); free(pending);

    CHECK(md_managed_store_backfill(s,MD_STORE_FAVORITES,true) == MD_STORE_OK);
    CHECK(md_managed_store_stage(s,MD_STORE_FAVORITES,"","[[\"p\",\"bad\"]]",targets,1,now) == MD_STORE_INVALID);
    char *oversize = malloc(MD_STORE_MAX_CONTENT+2); CHECK(oversize);
    memset(oversize,'x',MD_STORE_MAX_CONTENT+1); oversize[MD_STORE_MAX_CONTENT+1] = 0;
    CHECK(md_managed_store_stage(s,MD_STORE_FAVORITES,"",oversize,targets,1,now) == MD_STORE_LIMIT);
    CHECK(md_managed_store_receive(s,MD_STORE_FAVORITES,oversize,now) == MD_STORE_INVALID);
    free(oversize);
    json_t *large = json_array(); CHECK(large);
    for (size_t i = 0; i < MD_STORE_MAX_HOSTS; ++i) {
        char key[65]; snprintf(key,sizeof(key),"%064zx",i);
        CHECK(!json_array_append_new(large,json_pack("[s,s]","p",key)));
    }
    char *large_list = json_dumps(large,JSON_COMPACT);
    CHECK(large_list && strlen(large_list) > MD_STORE_MAX_PLAINTEXT);
    CHECK(md_managed_store_stage(s,MD_STORE_FAVORITES,"",large_list,targets,1,now) == MD_STORE_LIMIT);
    free(large_list); json_decref(large);
    a = make(client,PK,MD_STORE_MANAGEMENT,NOSTR_KIND_APPLICATION_SPECIFIC_DATA,V1,now+301);
    CHECK(md_managed_store_receive(s,MD_STORE_MANAGEMENT,a,now+1) == MD_STORE_OK); free(a);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,NULL,NULL) == MD_STORE_OK);
    CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,revision,edited,targets,1,now+1) == MD_STORE_CLOCK);

    for (int i = 0; i < 101; ++i) CHECK(md_managed_store_record(s,HOST,now+i,MD_RECENT_CONNECTED) == MD_STORE_OK);
    char *recent = md_managed_store_recents(s);
    json_t *history = json_loads(recent,0,NULL); free(recent);
    CHECK(json_array_size(history) == 100 && json_integer_value(json_object_get(json_array_get(history,99),"at")) == now+1);
    json_decref(history);
    char recent_path[1024]; journal_path(recent_path,sizeof(recent_path),argv[2],"local-recents");
    const char *const broken_recent[] = {"", "{", "[{\"at\":23}]"};
    for (size_t i = 0; i < sizeof(broken_recent)/sizeof(broken_recent[0]); ++i) {
        overwrite_file(recent_path,broken_recent[i]);
        CHECK(md_managed_store_record(s,OTHER,now,MD_RECENT_BIND) == MD_STORE_OK);
        recent = md_managed_store_recents(s); CHECK(recent);
        history = json_loads(recent,0,NULL); free(recent);
        CHECK(json_array_size(history) == 1);
        CHECK(!strcmp(json_string_value(json_object_get(json_array_get(history,0),"pubkey")),OTHER));
        json_decref(history);
    }
    CHECK(!md_managed_store_unattended(s));
    CHECK(md_managed_store_set_unattended(s,true) == MD_STORE_OK && md_managed_store_unattended(s));
    CHECK(md_managed_store_set_unattended(s,false) == MD_STORE_OK && !md_managed_store_unattended(s));
    check_admission(s,client,argv[2],targets,now);
    check_relay_outcomes(s,client,targets,now);
    check_normalized_receipts(s,now);
    check_keep_remote(s,client,argv[2],targets,now);
    check_exchange();
    check_offline_quick_hosts(s, binding, client, argv[2], targets, now + 302);
    CHECK(!md_accounts_bind_role(accounts,MD_ACCOUNT_CLIENT,backend,HOST_PK,other));
    CHECK(md_managed_store_read(s,MD_STORE_HOSTS,revision,NULL,NULL) == MD_STORE_REVOKED);
    CHECK(!md_managed_store_quick_hosts(binding, argv[2]));
    md_managed_store_close(s);
    md_account_binding_release(binding);
    binding = md_accounts_acquire_role(accounts,MD_ACCOUNT_CLIENT);
    CHECK(!md_managed_store_quick_hosts(binding, argv[2]));
    char absent[1024];
    CHECK(snprintf(absent, sizeof(absent), "%s/%s", argv[2], HOST_PK) > 0);
    CHECK(access(absent, F_OK) != 0);
    s = md_managed_store_open(binding,argv[2]); CHECK(s);
    CHECK(md_managed_store_read(s,MD_STORE_HOSTS,revision,&plain,NULL) == MD_STORE_OK && !*revision && !plain);
    CHECK(!md_managed_store_unattended(s));
    md_managed_store_close(s); md_account_binding_release(binding);
    md_accounts_destroy(accounts); md_signer_destroy(client); md_signer_destroy(other);
    md_nostr_shutdown();
    puts("PASS: managed store");
    return 0;
}
