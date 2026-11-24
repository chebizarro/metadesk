#include "relay_plan.h"
#include <nostr-event.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define SK "0000000000000000000000000000000000000000000000000000000000000001"
#define PK "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"

static NostrEvent *metadata(int kind, int created, const char *url) {
    NostrEvent *e = nostr_event_new();
    CHECK(e);
    nostr_event_set_pubkey(e, PK);
    nostr_event_set_kind(e, kind);
    nostr_event_set_created_at(e, created);
    nostr_event_set_content(e, "");
    NostrTags *tags = nostr_tags_new(0);
    CHECK(tags);
    if (kind == NOSTR_KIND_RELAY_LIST_METADATA) {
        nostr_tags_append(tags, nostr_tag_new("r", url, "read", NULL));
        nostr_tags_append(tags, nostr_tag_new("r", "wss://1.0.0.1", "write", NULL));
    } else {
        nostr_tags_append(tags, nostr_tag_new("relay", url, NULL));
    }
    nostr_event_set_tags(e, tags);
    CHECK(!nostr_event_sign(e, SK));
    return e;
}

static void expect(const MdRelayPlan *p, size_t i, const char *url, MdRelaySource source) {
    CHECK(i < p->count);
    CHECK(!strcmp(p->target[i].url, url));
    CHECK(p->target[i].source == source);
    CHECK(md_relay_target_check(&p->target[i], 0));
}

static void test_connection_budget(void) {
    MdRelayPlan roles[4] = {0};
    const unsigned masks[] = {1, 2, 4, 8};
    MdRelayUse union_targets[MD_RELAY_CONNECTION_MAX];
    for (size_t r = 0; r < 4; ++r) {
        for (size_t i = 0; i < MD_RELAY_TARGET_MAX; ++i) {
            char url[100];
            snprintf(url, sizeof(url), "wss://8.8.8.8/%zu/%zu", r, i);
            CHECK(md_relay_plan_add(&roles[r], url, MD_RELAY_NIP65));
        }
        CHECK(!md_relay_plan_add(&roles[r], "wss://8.8.4.4/extra", MD_RELAY_NIP65));
    }
    CHECK(md_relay_plan_union(roles, masks, 4, union_targets) == MD_RELAY_CONNECTION_MAX);
    for (size_t i = 0; i < MD_RELAY_CONNECTION_MAX; ++i)
        CHECK(union_targets[i].mask == masks[i % 4]);
    md_relay_plan_clear(&roles[1]);
    CHECK(md_relay_plan_add(&roles[1], roles[0].target[0].url, MD_RELAY_CONFIGURED));
    CHECK(md_relay_plan_union(roles, masks, 4, union_targets) == MD_RELAY_CONNECTION_MAX);
    CHECK(union_targets[0].mask == (masks[0] | masks[1]));
    CHECK(union_targets[0].target->source == MD_RELAY_CONFIGURED);
    for (size_t i = 0; i < 4; ++i) md_relay_plan_clear(&roles[i]);
}

int main(void) {
    test_connection_budget();
    MdRelayPlan p = {0}, bootstrap = {0};
    MdRelayMetadata m = {0};
    MdIdentity hints = {.hint_source = MD_HINT_NPROFILE, .hint_count = 1, .hints = {"wss://1.1.1.1/hint/"}};
    CHECK(md_relay_plan_build(&p, MD_RELAY_DISCOVERY, &bootstrap, &hints, &m) == MD_RELAY_NO_BOOTSTRAP);
    CHECK(!p.count);
    const char *invalid[] = {"wss://127.0.0.1", "wss://127.1", "wss://2130706433",
        "wss://localhost", "wss://10.2.3.4", "wss://172.16.4.5", "wss://192.168.1.1",
        "wss://169.254.1.1", "wss://0.0.0.0", "wss://100.64.0.1", "wss://100.127.255.255",
        "wss://[::1]", "wss://[fd00::1]", "wss://[fe80::1]", "wss://[2002:0808:0808::1]",
        "wss://[2001:0:1234::1]", "wss://[3fff:fff::1]", "wss://[64:ff9b::a00:1]",
        "wss://[64:ff9b::7f00:1]", "wss://[64:ff9b::6440:1]",
        "wss://[::ffff:10.0.0.1]", "wss://user@8.8.8.8", "wss://8.8.8.8/#x",
        "wss://8.8.8.8:0", "wss://8.8.8.8:65536", "ws://8.8.8.8", "http://8.8.8.8"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        MdRelayTarget t = {(char *)invalid[i], MD_RELAY_NIP05};
        CHECK(!md_relay_plan_add(&p, invalid[i], MD_RELAY_NIP05));
        CHECK(!md_relay_target_check(&t, 0));
    }
    MdRelayTarget expired = {"wss://8.8.8.8", MD_RELAY_NIP05};
    CHECK(!md_relay_target_check(&expired, 1));
    CHECK(md_relay_target_check(&expired, 0));
    MdRelayTarget nat64 = {"wss://[64:ff9b::808:808]", MD_RELAY_NIP05};
    CHECK(md_relay_target_check(&nat64, 0));
    CHECK(md_relay_plan_add(&bootstrap, "ws://127.0.0.1:80/", MD_RELAY_CONFIGURED));
    CHECK(md_relay_plan_add(&bootstrap, "ws://127.0.0.1", MD_RELAY_CONFIGURED));
    CHECK(bootstrap.count == 1);
    CHECK(md_relay_plan_build(&p, MD_RELAY_BOOTSTRAP, &bootstrap, &hints, &m) == MD_RELAY_READY);
    CHECK(p.count == 1);
    expect(&p, 0, "ws://127.0.0.1", MD_RELAY_CONFIGURED);
    md_relay_plan_clear(&p);
    CHECK(md_relay_plan_build(&p, MD_RELAY_DISCOVERY, &bootstrap, &hints, &m) == MD_RELAY_READY);
    CHECK(p.count == 2);
    expect(&p, 1, "wss://1.1.1.1/hint", MD_RELAY_NPROFILE);
    md_relay_plan_clear(&p);
    CHECK(md_relay_plan_build(&p, MD_RELAY_DM_IN, &bootstrap, &hints, &m) == MD_RELAY_NO_TARGETS);
    CHECK(md_relay_plan_build(&p, MD_RELAY_STORE, &bootstrap, NULL, &m) == MD_RELAY_READY);
    expect(&p, 0, "ws://127.0.0.1", MD_RELAY_CONFIGURED);
    md_relay_plan_clear(&p);

    NostrEvent *outbox = metadata(NOSTR_KIND_RELAY_LIST_METADATA, 2, "wss://8.8.8.8/read");
    NostrEvent *old = metadata(NOSTR_KIND_RELAY_LIST_METADATA, 1, "wss://8.8.4.4");
    NostrEvent *inbox = metadata(NOSTR_KIND_DM_RELAY_LIST, 3, "wss://9.9.9.9/dm");
    CHECK(!md_relay_metadata_offer(&m, "another author", outbox));
    CHECK(md_relay_metadata_offer(&m, PK, outbox));
    CHECK(!md_relay_metadata_offer(&m, PK, outbox));
    CHECK(!md_relay_metadata_offer(&m, PK, old));
    nostr_event_set_content(old, "bad signature");
    CHECK(!md_relay_metadata_offer(&m, PK, old));
    CHECK(md_relay_plan_build(&p, MD_RELAY_STORE, &bootstrap, NULL, &m) == MD_RELAY_READY);
    CHECK(p.count == 1);
    expect(&p, 0, "wss://1.0.0.1", MD_RELAY_NIP65);
    md_relay_plan_clear(&p);
    CHECK(md_relay_plan_build(&p, MD_RELAY_FETCH, &bootstrap, NULL, &m) == MD_RELAY_READY);
    CHECK(p.count == 3);
    expect(&p, 0, "ws://127.0.0.1", MD_RELAY_CONFIGURED);
    expect(&p, 1, "wss://8.8.8.8/read", MD_RELAY_NIP65);
    expect(&p, 2, "wss://1.0.0.1", MD_RELAY_NIP65);
    md_relay_plan_clear(&p);
    CHECK(md_relay_plan_build(&p, MD_RELAY_DM_OUT, &bootstrap, &hints, &m) == MD_RELAY_READY);
    CHECK(p.count == 1);
    expect(&p, 0, "wss://8.8.8.8/read", MD_RELAY_NIP65);
    md_relay_plan_clear(&p);
    CHECK(md_relay_metadata_offer(&m, PK, inbox));
    for (int role = MD_RELAY_DM_IN; role <= MD_RELAY_DM_OUT; ++role) {
        CHECK(md_relay_plan_build(&p, role, &bootstrap, &hints, &m) == MD_RELAY_READY);
        CHECK(p.count == 1);
        expect(&p, 0, "wss://9.9.9.9/dm", MD_RELAY_DM_LIST);
        md_relay_plan_clear(&p);
    }
    md_relay_metadata_clear(&m);
    NostrEvent *tie = metadata(NOSTR_KIND_DM_RELAY_LIST, 3, "wss://8.8.4.4/dm");
    char *a = nostr_event_get_id(inbox), *b = nostr_event_get_id(tie);
    CHECK(a && b && strcmp(a, b));
    NostrEvent *lowest = strcmp(a, b) < 0 ? inbox : tie;
    NostrEvent *highest = lowest == inbox ? tie : inbox;
    CHECK(md_relay_metadata_offer(&m, PK, highest));
    CHECK(md_relay_metadata_offer(&m, PK, lowest));
    CHECK(!md_relay_metadata_offer(&m, PK, highest));
    char *winner = nostr_event_get_id(m.dm);
    CHECK(winner && !strcmp(winner, lowest == inbox ? a : b));
    free(winner); free(a); free(b);
    md_relay_metadata_clear(&m);
    nostr_event_free(tie);
    nostr_event_free(inbox);
    nostr_event_free(old);
    nostr_event_free(outbox);
    hints.hint_source = MD_HINT_NIP05;
    CHECK(md_relay_plan_build(&p, MD_RELAY_DM_OUT, &bootstrap, &hints, &m) == MD_RELAY_READY);
    CHECK(p.count == 2);
    expect(&p, 0, "wss://1.1.1.1/hint", MD_RELAY_NIP05);
    expect(&p, 1, "ws://127.0.0.1", MD_RELAY_CONFIGURED);
    md_relay_plan_clear(&p);
    md_relay_plan_clear(&bootstrap);
    CHECK(md_relay_auth_from_json("{\"limitation\":{\"auth_required\":true}}") == MD_RELAY_AUTH_REQUIRED);
    CHECK(md_relay_auth_from_json("{\"limitation\":{\"auth_required\":false}}") == MD_RELAY_AUTH_NOT_ADVERTISED);
    CHECK(md_relay_auth_from_json("not JSON") == MD_RELAY_AUTH_UNKNOWN);
    return 0;
}
