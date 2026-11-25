#import "identity_model.h"
#import "hosts_model.h"
#include "fips_control.h"
#include "fips_addr.h"

static NSString *AccountID(const MdAccount *a) {
    return a ? [NSString stringWithFormat:@"%d:%s", (int)a->backend, a->pubkey] : @"";
}
NSArray<NSDictionary *> *MDIdentityAccounts(NSString *directory, NSString **error) {
    MdAccounts *accounts = NULL;
    int rc = md_accounts_open_snapshot(directory.fileSystemRepresentation, NULL, &accounts);
    if (rc) { if (error) *error = MDIdentityError(rc, MD_SIGNER_DIRECT_KEY); return nil; }
    NSMutableArray *rows = [NSMutableArray array];
    for (size_t i = 0; i < md_accounts_count(accounts); ++i) {
        const MdAccount *a = md_accounts_at(accounts, i);
        [rows addObject:@{@"id":AccountID(a), @"key":@(a->pubkey), @"backend":@(a->backend),
          @"transport":@(md_accounts_backend_name(a->backend)), @"label":MDHostBoundText(@(a->label),128),
          @"client":@(a == md_accounts_role(accounts, MD_ACCOUNT_CLIENT)), @"host":@(a == md_accounts_role(accounts, MD_ACCOUNT_HOST))}];
    }
    md_accounts_destroy(accounts); return rows;
}
NSString *MDIdentityError(int rc, MdSignerType backend) {
    switch (rc) {
        case MD_SIGNER_ERR_CANCELLED: return @"Verification cancelled. The saved role was not changed.";
        case MD_CREDENTIAL_LOCKED: return @"Keychain is locked. Unlock it externally and retry. Metadesk will not prompt or change the role.";
        case MD_CREDENTIAL_NOT_FOUND: return @"No saved NIP-46 pairing in Keychain. Restore the pairing with the account CLI.";
        case MD_CREDENTIAL_AMBIGUOUS: return @"More than one pairing matches in Keychain. Review or remove the stored pairing before retrying.";
        case MD_CREDENTIAL_INVALID: case MD_CREDENTIAL_FAILED: case MD_CREDENTIAL_UNAVAILABLE:
            return @"The local credential store could not complete the operation. No plaintext fallback is available.";
        case MD_ACCOUNTS_FIPS_KEY_MISMATCH: return @"Host binding refused: the selected key differs from the local FIPS node key.";
        case MD_SIGNER_ERR_KEY_CHANGED: return @"The signer returned a different public key. The saved role was not changed.";
        case MD_SIGNER_ERR_DENIED: return @"The signer denied verification. Unlock or approve in the external signer before retrying.";
        case MD_SIGNER_ERR_CONNECT: case MD_SIGNER_ERR_TIMEOUT: case MD_SIGNER_ERR_NO_KEY:
            if (backend == MD_SIGNER_NIP5F) return @"The local socket signer is unavailable. Start the signer, then retry.";
            if (backend == MD_SIGNER_NIP55L) return @"The external signer is unavailable. Unlock in signer if locked, then retry.";
            if (backend == MD_SIGNER_NIP46) return @"Remote signer unavailable or approval did not arrive. Check the remote signer and retry.";
            break;
        default: break;
    }
    return [NSString stringWithFormat:@"Account operation failed: %s", md_accounts_error_string(rc)];
}
BOOL MDIdentityLocalNode(char out[65], void *unused) {
    (void)unused;
    MdFipsControlResponse response = {0};
    MdFipsControlResult rc = md_fips_control_request(NULL, "show_status", NULL, 5000, &response);
    cJSON *npub = response.data ? cJSON_GetObjectItemCaseSensitive(response.data, "npub") : NULL;
    BOOL ok = rc == MD_FIPS_CONTROL_OK && cJSON_IsString(npub) && !md_fips_npub_to_pubkey_hex(npub->valuestring,out,65);
    md_fips_control_response_free(&response); return ok;
}
NSString *MDIdentityBind(NSString *directory, MdCredentialStore *store, MdAccountRole role,
    NSDictionary *account, NSString *expected, BOOL fips, MDIdentityConnect connect,
    MDIdentityNode node, MDIdentityCancelled cancelled, void *context) {
    MdSignerType backend = [account[@"backend"] intValue];
    NSString *key = account[@"key"];
    if ((role != MD_ACCOUNT_CLIENT && role != MD_ACCOUNT_HOST) || !expected ||
        (account && (!MDHostFingerprint(key) || (backend != MD_SIGNER_NIP5F && backend != MD_SIGNER_NIP55L && backend != MD_SIGNER_NIP46))))
        return @"Choose an external signer account.";
    if (cancelled && cancelled(context)) return MDIdentityError(MD_SIGNER_ERR_CANCELLED,backend);
    char nodeKey[65] = {0};
    if (account && role == MD_ACCOUNT_HOST && fips) {
        if (!(node ?: MDIdentityLocalNode)(nodeKey,context) || !MDHostFingerprint(@(nodeKey)))
            return @"Host binding refused: no verified node key from the local FIPS daemon (show_status). Start it and retry, or choose direct-IP hosting explicitly.";
        if (strcmp(nodeKey,key.UTF8String)) return MDIdentityError(MD_ACCOUNTS_FIPS_KEY_MISMATCH,backend);
    }
    MdSigner *signer = NULL;
    int rc = account ? (connect ?: md_accounts_connect_signer)(backend,key.UTF8String,store,&signer) : 0;
    if (cancelled && cancelled(context)) rc = MD_SIGNER_ERR_CANCELLED;
    MdAccounts *a = NULL;
    if (!rc) rc = md_accounts_open(directory.fileSystemRepresentation,store,&a);
    if (!rc && ![expected isEqual:AccountID(md_accounts_role(a,role))]) {
        md_signer_destroy(signer); md_accounts_destroy(a);
        return @"The saved role changed elsewhere. Reload accounts and review the new selection.";
    }
    if (!rc) rc = account ? md_accounts_bind_role(a,role,backend,key.UTF8String,signer) : md_accounts_unbind_role(a,role);
    if (!rc && account && role == MD_ACCOUNT_HOST && fips) rc = md_accounts_check_fips_host(a,nodeKey);
    if (!rc && cancelled && cancelled(context)) rc = MD_SIGNER_ERR_CANCELLED;
    if (!rc) rc = md_accounts_save(a);
    md_accounts_destroy(a); md_signer_destroy(signer);
    return rc ? MDIdentityError(rc,backend) : @"";
}
NSString *MDIdentityRemove(NSString *directory, MdCredentialStore *store, NSDictionary *account) {
    NSString *key = account[@"key"];
    MdSignerType backend = [account[@"backend"] intValue];
    if (!MDHostFingerprint(key)) return @"Choose a saved signer account.";
    MdAccounts *a = NULL; int rc = md_accounts_open(directory.fileSystemRepresentation,store,&a);
    if (!rc) rc = md_accounts_remove(a,backend,key.UTF8String);
    if (!rc && md_accounts_save(a)) {
        md_accounts_destroy(a);
        return @"Could not save account removal. A local pairing may already have been deleted; repair the accounts store before retrying.";
    }
    md_accounts_destroy(a);
    return rc ? MDIdentityError(rc,backend) : @"";
}
