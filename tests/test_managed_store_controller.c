#include "managed_store_controller.h"
#include "md_time.h"
#include "nostr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
#define HOST "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define MEMBERS "[[\"p\",\"" HOST "\"]]"

int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    MdAccounts *accounts = NULL;
    MdSigner *signer = md_signer_create_direct("0000000000000000000000000000000000000000000000000000000000000002");
    char *key = NULL;
    CHECK(signer && !md_signer_get_pubkey(signer, &key));
    if (argc == 2) {
        CHECK(!strcmp(argv[1], "future-event"));
        NostrEvent *event = md_private_document_build(signer, NULL, key, 30000,
            MD_STORE_HOSTS, MEMBERS, time(NULL) + 3600);
        char *wire = NULL;
        CHECK(event && !md_nostr_sign_event_json(signer, event, &wire));
        puts(wire);
        free(wire); nostr_event_free(event); free(key); md_signer_destroy(signer);
        md_nostr_shutdown();
        return 0;
    }
    CHECK(!md_accounts_open(argv[1], NULL, &accounts));
    CHECK(!md_accounts_add(accounts, MD_SIGNER_DIRECT_KEY, key, "Fixture"));
    CHECK(!md_accounts_bind_role(accounts, MD_ACCOUNT_CLIENT, MD_SIGNER_DIRECT_KEY, key, signer));
    MdAccountBinding *binding = md_accounts_acquire_role(accounts, MD_ACCOUNT_CLIENT);
    MdStoreController *c = md_store_controller_open(binding, argv[1], NULL, NULL);
    CHECK(c && md_store_controller_sync(c, MD_STORE_HOSTS) == MD_SYNC_OFFLINE);
    MdRelayPlan plan = {0};
    CHECK(md_relay_plan_add(&plan, argv[2], MD_RELAY_CONFIGURED));
    CHECK(md_store_controller_start(c, &plan));
    CHECK(md_store_controller_wait(c, MD_STORE_HOSTS, md_monotonic_ns() + 5000000000ULL) == MD_SYNC_READY);
    CHECK(md_store_controller_wait(c, MD_STORE_FAVORITES, md_monotonic_ns() + 5000000000ULL) == MD_SYNC_REFUSED);
    CHECK(md_store_controller_edit(c, MD_STORE_FAVORITES, MD_EDIT_STAGE, "", MEMBERS) == MD_STORE_NOT_READY);
    CHECK(md_store_controller_edit(c, MD_STORE_HOSTS, MD_EDIT_STAGE, "", MEMBERS) == MD_STORE_PENDING);
    CHECK(md_store_controller_flush(c, MD_STORE_HOSTS) == MD_STORE_PENDING);
    char rev[65], *winner = NULL, *draft = NULL;
    CHECK(md_store_controller_read(c, MD_STORE_HOSTS, rev, &winner, &draft) == MD_STORE_PENDING);
    CHECK(!*rev && !winner && draft && !strcmp(draft, MEMBERS));
    free(draft);
    CHECK(md_store_controller_flush(c, MD_STORE_HOSTS) == MD_STORE_OK);
    CHECK(md_store_controller_start(c, NULL));
    CHECK(md_store_controller_sync(c, MD_STORE_HOSTS) == MD_SYNC_OFFLINE);
    CHECK(md_store_controller_read(c, MD_STORE_HOSTS, rev, &winner, NULL) == MD_STORE_OK);
    CHECK(*rev && winner && !strcmp(winner, MEMBERS));
    free(winner);
    CHECK(md_store_controller_edit(c, MD_STORE_HOSTS, MD_EDIT_STAGE, rev, "[]") == MD_STORE_NOT_READY);
    md_store_controller_close(c);
    md_relay_plan_clear(&plan);
    md_account_binding_release(binding);
    md_accounts_destroy(accounts);
    md_signer_destroy(signer);
    free(key);
    md_nostr_shutdown();
    return 0;
}
