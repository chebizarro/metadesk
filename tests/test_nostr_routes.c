#include "nostr.h"
#include "md_time.h"
#include <nostr-kinds.h>
#include <json.h>
#include <nostr-tag.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SK "0000000000000000000000000000000000000000000000000000000000000001"
#define PEER_SK "0000000000000000000000000000000000000000000000000000000000000002"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char *event_json(int kind, const char *sk, const char *url) {
    NostrEvent *event = nostr_event_new();
    CHECK(event);
    char *pk = md_nostr_get_pubkey(sk);
    CHECK(pk);
    nostr_event_set_pubkey(event, pk);
    free(pk);
    nostr_event_set_kind(event, kind);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_content(event, "");
    if (url) {
        NostrTags *tags = nostr_tags_new(0);
        CHECK(tags);
        NostrTag *tag = kind == NOSTR_KIND_DM_RELAY_LIST ? nostr_tag_new("relay", url, NULL) :
                                                        nostr_tag_new("r", url, "write", NULL);
        CHECK(tag);
        nostr_tags_append(tags, tag);
        nostr_event_set_tags(event, tags);
    }
    CHECK(!nostr_event_sign(event, sk));
    char *json = nostr_event_serialize(event);
    nostr_event_free(event);
    CHECK(json);
    return json;
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    const char *url;
    bool refused;
} Inbox;

static void dm(const char *sender, const char *content, const MdNostrDmInfo *info, void *data) {
    (void)sender; (void)content; (void)info; (void)data;
}

static void refused(const char *url, const char *reason, void *data) {
    Inbox *inbox = data;
    CHECK((!inbox->url || !strcmp(url, inbox->url)) && reason &&
          !strcmp(reason, "restricted: fixture inbox refusal"));
    pthread_mutex_lock(&inbox->mu);
    inbox->refused = true;
    pthread_cond_signal(&inbox->cv);
    pthread_mutex_unlock(&inbox->mu);
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "events")) {
        char *events[] = {event_json(NOSTR_KIND_DM_RELAY_LIST, SK, argv[2]),
            event_json(NOSTR_KIND_DM_RELAY_LIST, PEER_SK, argv[3]),
            event_json(NOSTR_KIND_RELAY_LIST_METADATA, SK, argv[4])};
        for (size_t i = 0; i < 3; ++i) { puts(events[i]); free(events[i]); }
        return 0;
    }
    bool fallback = argc == 7 && !strcmp(argv[6], "fallback");
    CHECK(argc == 6 || fallback);
    const char **urls = (const char **)&argv[1];
    const int count = 5;
    MdRelayPlan bootstrap = {0};
    for (int i = 0; i < count; ++i)
        CHECK(md_relay_plan_add(&bootstrap, urls[i], MD_RELAY_CONFIGURED));
    char *peer_key = md_nostr_get_pubkey(PEER_SK), *other = md_nostr_get_pubkey(SK);
    CHECK(peer_key && other);
    MdIdentity peer = {0};
    memcpy(peer.pubkey, peer_key, sizeof(peer.pubkey));
    Inbox inbox = {.mu = PTHREAD_MUTEX_INITIALIZER, .url = fallback ? NULL : urls[2]};
    CHECK(!md_cond_init_monotonic(&inbox.cv));
    MdNostrCallbacks cbs = {.on_dm = dm, .dm_userdata = &inbox, .on_dm_relay_unavailable = refused};
    MdNostrConfig cfg = {.sk_hex = SK, .relay_urls = urls, .relay_count = count,
        .bootstrap_plan = &bootstrap, .session_peer = &peer};
    MdNostr *bridge = md_nostr_create(&cfg, &cbs);
    CHECK(bridge);
    CHECK(md_nostr_send_session_request(bridge, other, "{}") == -1);
    CHECK(md_nostr_send_session_request(bridge, peer_key, "{}") == 0);
    char *doc = event_json(NOSTR_KIND_TEXT_NOTE, SK, NULL);
    CHECK(md_nostr_publish_signed_json(bridge, doc) == 0);
    free(doc);
    uint64_t deadline = md_monotonic_ns() + 5000000000ULL;
    pthread_mutex_lock(&inbox.mu);
    while (!inbox.refused && !md_cond_timedwait_until(&inbox.cv, &inbox.mu, deadline)) {}
    CHECK(inbox.refused);
    pthread_mutex_unlock(&inbox.mu);
    md_nostr_destroy(bridge);
    md_nostr_shutdown();
    pthread_cond_destroy(&inbox.cv);
    pthread_mutex_destroy(&inbox.mu);
    md_relay_plan_clear(&bootstrap);
    free(peer_key); free(other);
    return 0;
}
