#include "identity_resolver.h"
#include "discovery_http.h"
#include <nostr/nip19/nip19.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define PK "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
#define OTHER "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"

int main(void) {
    MdIdentity id = {0};
    CHECK(md_identity_resolve("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789", PK, &id) == MD_RESOLVE_OK);
    CHECK(!strcmp(id.pubkey, PK));
    char *npub = md_identity_npub(&id);
    CHECK(npub && !strncmp(npub, "npub1", 5));
    md_identity_clear(&id);
    char uri[128];
    snprintf(uri, sizeof(uri), "nostr:%s", npub);
    CHECK(md_identity_resolve(uri, PK, &id) == MD_RESOLVE_OK);
    CHECK(!strcmp(id.pubkey, PK) && !id.hint_count);
    md_identity_clear(&id);

    char *hints[] = {"wss://8.8.8.8/path"};
    NostrProfilePointer profile = {.public_key = PK, .relays = hints, .relays_count = 1};
    char *bech = NULL;
    CHECK(!nostr_nip19_encode_nprofile(&profile, &bech));
    CHECK(md_identity_resolve(bech, npub, &id) == MD_RESOLVE_OK);
    CHECK(!strcmp(id.pubkey, PK) && id.hint_source == MD_HINT_NPROFILE && id.hint_count == 1);
    CHECK(!strcmp(id.hints[0], hints[0]));
    md_identity_clear(&id);
    CHECK(md_identity_resolve(bech, OTHER, &id) == MD_RESOLVE_PIN_MISMATCH && !id.pubkey[0]);
    free(bech);
    free(npub);

    const char *json = "{\"names\":{\"alice\":\"" PK "\"},\"relays\":{\"" PK "\":[\"wss://9.9.9.9\"]}}";
    CHECK(md_identity_from_nip05_json("Alice@EXAMPLE.org", json, PK, &id) == MD_RESOLVE_OK);
    CHECK(!strcmp(id.pubkey, PK) && id.hint_source == MD_HINT_NIP05 && id.hint_count == 1);
    md_identity_clear(&id);
    CHECK(md_identity_from_nip05_json("Alice@example.org", json, OTHER, &id) == MD_RESOLVE_PIN_MISMATCH);
    CHECK(!id.pubkey[0] && !id.hint_count);
    CHECK(md_identity_from_nip05_json("Bob@example.org", json, PK, &id) == MD_RESOLVE_NOT_FOUND);
    CHECK(md_identity_resolve("127.0.0.1", NULL, &id) == MD_RESOLVE_INVALID);
    CHECK(md_identity_resolve("nsec1not-a-host", NULL, &id) == MD_RESOLVE_INVALID);
    CHECK(md_identity_resolve("alice@127.0.0.1", NULL, &id) == MD_RESOLVE_UNSAFE);
    bool unsafe = false;
    char *body = md_discovery_http_get("https://[::ffff:127.0.0.1]/", NULL, true, 1000, &unsafe);
    CHECK(!body && unsafe); /* Checked before connect, no listener or TLS fixture needed. */
#ifndef _WIN32
    CHECK(!setenv("NIP05_ALLOW_INSECURE", "1", 1));
    CHECK(!setenv("NIP05_TIMEOUT_MS", "1", 1));
    md_identity_scrub_environment();
    CHECK(!getenv("NIP05_ALLOW_INSECURE") && !getenv("NIP05_TIMEOUT_MS"));
#endif
    return 0;
}
