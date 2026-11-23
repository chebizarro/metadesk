#include "accounts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

static int usage(void) {
    fprintf(stderr,
        "Usage: metadesk-host account [--config-dir ABSOLUTE_DIRECTORY] COMMAND\n"
        "  list\n"
        "  add BACKEND PUBKEY [LABEL]\n"
        "  label BACKEND PUBKEY LABEL\n"
        "  remove BACKEND PUBKEY\n"
        "  credentials-stdin PUBKEY     (import saved NIP-46 pairing JSON)\n"
        "  bind-role client|host BACKEND PUBKEY [--secret-stdin] [--fips-node PUBKEY]\n"
        "  unbind-role client|host\n"
        "  check-fips-host NODE_PUBKEY\n"
        "Backends: direct, nip46, nip55l, nip5f. Pubkeys are lowercase hex.\n"
        "Direct binding requires --secret-stdin (explicit integration only).\n"
        "Credentials/keys must be piped on stdin, never placed in argv.\n"
        "Bound roles are restored at the next host/client startup unless an explicit\n"
        "signer option (--bunker, --dbus-signer, --socket-signer, --auto-signer) overrides them.\n");
    return 2;
}
static int parse_role(const char *s, MdAccountRole *role) {
    if (!strcmp(s, "client")) *role = MD_ACCOUNT_CLIENT;
    else if (!strcmp(s, "host")) *role = MD_ACCOUNT_HOST;
    else return MD_ACCOUNTS_INVALID;
    return 0;
}
static int read_private_input(char **out) {
    *out = NULL;
    if (isatty(fileno(stdin))) {
        fprintf(stderr, "Pipe private input from a trusted source; terminal echo and argv are not allowed.\n");
        return MD_ACCOUNTS_INVALID;
    }
    char *buffer = calloc(65538, 1);
    if (!buffer) return MD_ACCOUNTS_IO;
    size_t n = fread(buffer, 1, 65537, stdin);
    int rc = MD_ACCOUNTS_INVALID;
    if (!ferror(stdin) && n > 0 && n <= 65536 && !memchr(buffer, 0, n)) {
        *out = malloc(n + 1);
        if (*out) { memcpy(*out, buffer, n); (*out)[n] = 0; rc = 0; }
        else rc = MD_ACCOUNTS_IO;
    }
    md_credential_wipe(buffer, 65538); free(buffer); return rc;
}
int md_host_account_cli(int argc, char **argv) {
    const char *dir = NULL;
    if (argc >= 2 && !strcmp(argv[0], "--config-dir")) { dir = argv[1]; argc -= 2; argv += 2; }
    if (!argc || !strcmp(argv[0], "--help")) return usage();
    MdCredentialStore *store = md_credential_store_default();
    MdAccounts *a = NULL;
    int rc = md_accounts_open(dir, store, &a);
    if (rc) goto done;
    bool save = false;
    const char *cmd = argv[0];
    if (!strcmp(cmd, "list") && argc == 1) {
        for (size_t i = 0; i < md_accounts_count(a); ++i) {
            const MdAccount *info = md_accounts_at(a, i);
            printf("%s\t%s\t%s", md_accounts_backend_name(info->backend), info->pubkey, info->label);
            if (md_accounts_role(a, MD_ACCOUNT_CLIENT) == info) printf("\tclient");
            if (md_accounts_role(a, MD_ACCOUNT_HOST) == info) printf("\thost");
            putchar('\n');
        }
    } else if ((!strcmp(cmd, "add") && (argc == 3 || argc == 4)) ||
               (!strcmp(cmd, "label") && argc == 4) || (!strcmp(cmd, "remove") && argc == 3)) {
        MdSignerType type;
        rc = md_accounts_parse_backend(argv[1], &type);
        if (!rc) {
            if (!strcmp(cmd, "add")) rc = md_accounts_add(a, type, argv[2], argc == 4 ? argv[3] : "");
            else if (!strcmp(cmd, "label")) rc = md_accounts_label(a, type, argv[2], argv[3]);
            else rc = md_accounts_remove(a, type, argv[2]);
            save = !rc;
        }
    } else if (!strcmp(cmd, "credentials-stdin") && argc == 2) {
        bool exists = false;
        for (size_t i = 0; i < md_accounts_count(a); ++i) {
            const MdAccount *info = md_accounts_at(a, i);
            if (info->backend == MD_SIGNER_NIP46 && !strcmp(info->pubkey, argv[1])) exists = true;
        }
        rc = exists ? 0 : MD_ACCOUNTS_NOT_FOUND;
        char *json = NULL; MdNip46Credentials *c = NULL;
        if (!rc) rc = read_private_input(&json);
        if (!rc) rc = md_nip46_credentials_parse(json, &c);
        md_credential_free(json);
        if (!rc && strcmp(c->user_pubkey, argv[1])) rc = MD_SIGNER_ERR_KEY_CHANGED;
        if (!rc) rc = md_credential_store_put(store, c);
        md_nip46_credentials_free(c);
    } else if (!strcmp(cmd, "bind-role") && argc >= 4) {
        MdAccountRole role; MdSignerType type;
        rc = parse_role(argv[1], &role);
        if (!rc) rc = md_accounts_parse_backend(argv[2], &type);
        bool secret_stdin = false; const char *node = NULL;
        for (int i = 4; !rc && i < argc; ++i) {
            if (!strcmp(argv[i], "--secret-stdin") && !secret_stdin) secret_stdin = true;
            else if (!strcmp(argv[i], "--fips-node") && !node && i + 1 < argc) node = argv[++i];
            else rc = MD_ACCOUNTS_INVALID;
        }
        if (!rc && ((secret_stdin && type != MD_SIGNER_DIRECT_KEY) || (node && role != MD_ACCOUNT_HOST)))
            rc = MD_ACCOUNTS_INVALID;
        if (!rc && node && (strlen(node) != 64 || strspn(node, "0123456789abcdef") != 64)) rc = MD_ACCOUNTS_INVALID;
        if (!rc && node && strcmp(node, argv[3])) rc = MD_ACCOUNTS_FIPS_KEY_MISMATCH;
        MdSigner *s = NULL;
        if (!rc && type == MD_SIGNER_DIRECT_KEY) {
            char *secret = NULL;
            rc = secret_stdin ? read_private_input(&secret) : MD_ACCOUNTS_NOT_READY;
            if (!rc) {
                size_t n = strlen(secret);
                while (n && (secret[n - 1] == '\n' || secret[n - 1] == '\r')) secret[--n] = 0;
                if (n == 64 && strspn(secret, "0123456789abcdef") == 64) s = md_signer_create_direct(secret);
                rc = s ? 0 : MD_ACCOUNTS_INVALID;
            }
            md_credential_free(secret);
        } else if (!rc) rc = md_accounts_connect_signer(type, argv[3], store, &s);
        if (!rc) rc = md_accounts_bind_role(a, role, type, argv[3], s);
        md_signer_destroy(s); save = !rc;
    } else if (!strcmp(cmd, "unbind-role") && argc == 2) {
        MdAccountRole role;
        rc = parse_role(argv[1], &role);
        if (!rc) rc = md_accounts_unbind_role(a, role);
        save = !rc;
    } else if (!strcmp(cmd, "check-fips-host") && argc == 2) {
        rc = md_accounts_check_fips_host(a, argv[1]);
    } else { rc = MD_ACCOUNTS_INVALID; usage(); }
    if (save) {
        rc = md_accounts_save(a);
        if (rc) fprintf(stderr, "Account changes were NOT saved; rerun after fixing metadata storage.\n");
    }
done:
    if (rc) fprintf(stderr, "account: %s\n", md_accounts_error_string(rc));
    md_accounts_destroy(a); md_credential_store_destroy(store);
    return rc ? 1 : 0;
}
