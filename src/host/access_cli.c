#include "accounts.h"
#include "fips_addr.h"
#include "nostr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fprintf(stderr,
        "Usage: metadesk-host access [--config-dir ABSOLUTE_DIRECTORY] [--relay URL ...] COMMAND\n"
        "  list\n"
        "  add <npub>\n"
        "  remove <npub>\n"
        "Uses the saved host role; no signer override or open mode.\n"
        "Membership permits requested video, agent and input capabilities.\n"
        "Per-client capability restrictions are not implemented; caps arguments are rejected.\n"
        "list prints public keys in lowercase hex, one per line.\n"
        "Waits for relay history and, on edits, a positive publish acknowledgement.\n");
    return 2;
}

int md_host_access_cli(int argc, char **argv) {
    const char *dir = NULL;
    const char **relays = calloc((size_t)argc + 1, sizeof(*relays));
    if (!relays) return 1;
    int relay_count = 0, arg = 0;
    while (arg < argc && argv[arg][0] == '-') {
        if (!strcmp(argv[arg], "--config-dir") && !dir && arg + 1 < argc)
            dir = argv[++arg];
        else if (!strcmp(argv[arg], "--relay") && arg + 1 < argc)
            relays[relay_count++] = argv[++arg];
        else { free(relays); return usage(); }
        ++arg;
    }
    int remaining = argc - arg;
    const char *cmd = remaining ? argv[arg] : "";
    bool list = !strcmp(cmd, "list"), add = !strcmp(cmd, "add");
    char pk[65] = {0};
    if ((list && remaining != 1) ||
        (!list && ((!add && strcmp(cmd, "remove")) || remaining != 2)) ||
        (!list && md_fips_npub_to_pubkey_hex(argv[arg + 1], pk, sizeof(pk)))) {
        free(relays);
        return usage();
    }
    if (!relay_count) relays[relay_count++] = MD_DEFAULT_RELAY;

    MdCredentialStore *store = md_credential_store_default();
    MdAccounts *accounts = NULL;
    MdAccountBinding *binding = NULL;
    MdNostr *nostr = NULL;
    int rc = md_accounts_open_snapshot(dir, store, &accounts);
    if (!rc) rc = md_accounts_restore_role(accounts, MD_ACCOUNT_HOST, &binding);
    if (rc || !binding) {
        fprintf(stderr, "access: host account restore: %s\n",
                md_accounts_error_string(rc ? rc : MD_ACCOUNTS_NOT_FOUND));
        if (!rc) rc = MD_ACCOUNTS_NOT_FOUND;
        goto done;
    }
    MdNostrConfig cfg = {
        .signer = md_account_binding_signer(binding), .owner = binding,
        .relay_urls = relays, .relay_count = relay_count,
    };
    nostr = md_nostr_create(&cfg, NULL);
    if (!nostr) {
        fprintf(stderr, "access: could not start host relay bridge\n");
        rc = -1;
        goto done;
    }
    if (list) {
        MdAllowlistEntry *entries = NULL;
        size_t count = 0;
        rc = md_nostr_allowlist_snapshot(nostr, &entries, &count);
        if (!rc) for (size_t i = 0; i < count; ++i) puts(entries[i].pubkey_hex);
        free(entries);
        if (!rc && fflush(stdout)) rc = -1;
    } else {
        rc = add ? md_nostr_allowlist_add(nostr, pk, NULL)
                 : md_nostr_allowlist_remove(nostr, pk);
    }
    if (rc) fprintf(stderr, "access: operation failed (history unavailable, signer/publish failure, or concurrent replacement); reread list before retrying\n");
done:
    md_nostr_destroy(nostr);
    md_account_binding_release(binding);
    md_accounts_destroy(accounts);
    md_credential_store_destroy(store);
    free(relays);
    return rc ? 1 : 0;
}
