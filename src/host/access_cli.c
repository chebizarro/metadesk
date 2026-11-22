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
        "  add <npub> [caps]\n"
        "  remove <npub>\n"
        "  migrate [<npub> <caps> ...]  (confirm EVERY source member)\n"
        "Uses the saved host role; no signer override or open mode.\n"
        "caps: none or a comma-separated subset in video,agent,input,monitor,clipboard order (no spaces).\n"
        "Explicit caps replaces the grant; omitted caps preserves existing, defaults new to video.\n"
        "Legacy/mixed lists deny access and require explicit migration.\n"
        "Migration cannot erase historical public copies on relays or observers.\n"
        "list prints lowercase hex public keys and effective caps, tab-separated.\n"
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
    bool migrate = !strcmp(cmd, "migrate");
    char pk[65] = {0};
    const char *caps = add && remaining == 3 ? argv[arg + 2] : NULL;
    uint32_t parsed;
    if ((list && remaining != 1) ||
        (!list && !migrate && ((!add && strcmp(cmd, "remove")) ||
                   (add ? remaining != 2 && remaining != 3 : remaining != 2))) ||
        (migrate && remaining % 2 != 1) ||
        (caps && md_nostr_parse_caps(caps, &parsed)) ||
        (!list && !migrate && md_fips_npub_to_pubkey_hex(argv[arg + 1], pk, sizeof(pk)))) {
        free(relays);
        return usage();
    }
    size_t confirmed_count = migrate ? (size_t)(remaining - 1) / 2 : 0;
    MdAllowlistEntry *confirmed = confirmed_count ? calloc(confirmed_count, sizeof(*confirmed)) : NULL;
    if (confirmed_count && !confirmed) { free(relays); return 1; }
    for (size_t i = 0; i < confirmed_count; i++) {
        const char *scope = argv[arg + 2 + 2 * i];
        if (md_fips_npub_to_pubkey_hex(argv[arg + 1 + 2 * i], confirmed[i].pubkey_hex, 65) ||
            md_nostr_parse_caps(scope, &parsed)) {
            free(confirmed); free(relays); return usage();
        }
        snprintf(confirmed[i].caps, sizeof(confirmed[i].caps), "%s", scope);
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
        if (!rc) for (size_t i = 0; i < count; ++i)
            printf("%s\t%s%s\n", entries[i].pubkey_hex, entries[i].caps,
                   entries[i].review_required ? "\tlegacy-public-review-required" : "");
        free(entries);
        if (!rc && fflush(stdout)) rc = -1;
    } else if (migrate) {
        fprintf(stderr, "access migrate: confirming all listed principals/scopes; historical public copies cannot be erased\n");
        for (size_t i = 0; i < confirmed_count; i++)
            fprintf(stderr, "%s\t%s\n", confirmed[i].pubkey_hex, confirmed[i].caps);
        rc = md_nostr_allowlist_migrate(nostr, confirmed, confirmed_count);
    } else {
        rc = add ? md_nostr_allowlist_add(nostr, pk, caps)
                 : md_nostr_allowlist_remove(nostr, pk);
    }
    if (rc) fprintf(stderr, "access: operation failed (history unavailable, signer/publish failure, concurrent replacement, or review-required policy: use migrate with every member and explicit caps); reread list before retrying\n");
done:
    md_nostr_destroy(nostr);
    md_account_binding_release(binding);
    md_accounts_destroy(accounts);
    md_credential_store_destroy(store);
    free(confirmed);
    free(relays);
    return rc ? 1 : 0;
}
