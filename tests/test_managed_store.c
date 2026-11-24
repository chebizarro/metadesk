#include "managed_store.h"
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
int main(int argc, char **argv) {
    CHECK(argc == 8);
    const char *targets[] = {argv[5],argv[6]};
    int64_t now = (int64_t)time(NULL);
    MdSigner *client = md_signer_create_nip5f(argv[3]);
    MdSigner *other = md_signer_create_nip5f(argv[4]);
    CHECK(client && other);
    MdAccounts *accounts = NULL;
    CHECK(!md_accounts_open(argv[1],NULL,&accounts));
    CHECK(!md_accounts_add(accounts,MD_SIGNER_NIP5F,PK,"client"));
    CHECK(!md_accounts_add(accounts,MD_SIGNER_NIP5F,HOST_PK,"other"));
    CHECK(!md_accounts_bind_role(accounts,MD_ACCOUNT_CLIENT,MD_SIGNER_NIP5F,PK,client));
    CHECK(!md_accounts_bind_role(accounts,MD_ACCOUNT_HOST,MD_SIGNER_NIP5F,HOST_PK,other));
    MdAccountBinding *host = md_accounts_acquire_role(accounts,MD_ACCOUNT_HOST);
    CHECK(host && !md_managed_store_open(host,argv[2]));
    md_account_binding_release(host);
    MdAccountBinding *binding = md_accounts_acquire_role(accounts,MD_ACCOUNT_CLIENT);
    MdManagedStore *s = md_managed_store_open(binding,argv[2]);
    CHECK(s && !md_managed_store_open(binding,argv[2]));
    bool crash = !strcmp(argv[7],"crash");
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
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,&plain,&pending) == MD_STORE_CONFLICT);
    CHECK(strstr(plain,"different layout") && strstr(pending,"edited"));
    free(plain); free(pending); free(remote);
    CHECK(md_managed_store_outgoing(s,MD_STORE_MANAGEMENT,targets[0],&wire) == MD_STORE_CONFLICT && !wire);
    CHECK(md_managed_store_keep_remote(s,MD_STORE_MANAGEMENT) == MD_STORE_OK);
    CHECK(md_managed_store_read(s,MD_STORE_MANAGEMENT,revision,NULL,NULL) == MD_STORE_READ_ONLY);
    CHECK(md_managed_store_stage(s,MD_STORE_MANAGEMENT,revision,V1,targets,1,now) == MD_STORE_READ_ONLY);

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
    CHECK(!md_managed_store_unattended(s));
    CHECK(md_managed_store_set_unattended(s,true) == MD_STORE_OK && md_managed_store_unattended(s));
    CHECK(md_managed_store_set_unattended(s,false) == MD_STORE_OK && !md_managed_store_unattended(s));
    check_exchange();
    CHECK(!md_accounts_bind_role(accounts,MD_ACCOUNT_CLIENT,MD_SIGNER_NIP5F,HOST_PK,other));
    CHECK(md_managed_store_read(s,MD_STORE_HOSTS,revision,NULL,NULL) == MD_STORE_REVOKED);
    md_managed_store_close(s);
    md_account_binding_release(binding);
    binding = md_accounts_acquire_role(accounts,MD_ACCOUNT_CLIENT);
    s = md_managed_store_open(binding,argv[2]); CHECK(s);
    CHECK(md_managed_store_read(s,MD_STORE_HOSTS,revision,&plain,NULL) == MD_STORE_OK && !*revision && !plain);
    CHECK(!md_managed_store_unattended(s));
    md_managed_store_close(s); md_account_binding_release(binding);
    md_accounts_destroy(accounts); md_signer_destroy(client); md_signer_destroy(other);
    md_nostr_shutdown();
    puts("PASS: managed store");
    return 0;
}
