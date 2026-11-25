#import "identity_model.h"
#import "hosts_model.h"
#include "fips_control.h"
#include "fips_addr.h"
#include <nostr/nip19/nip19.h>

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
        case MD_CREDENTIAL_NOT_FOUND: return @"No saved NIP-46 pairing in Keychain. Import the saved pairing in Identity or with the account CLI.";
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
BOOL MDIdentitySupportsBackend(MdSignerType backend) {
    switch (backend) {
#ifdef MD_SIGNER_ENABLE_NIP5F
        case MD_SIGNER_NIP5F: return YES;
#endif
#ifdef MD_SIGNER_ENABLE_NIP46
        case MD_SIGNER_NIP46: return YES;
#endif
#ifdef MD_SIGNER_ENABLE_NIP55L
        case MD_SIGNER_NIP55L: return YES;
#endif
        default: return NO;
    }
}

static NSString *PublicKey(NSString *text) {
    NSString *key = MDHostTrim(text);
    if (key.length > 64) return nil;
    const char *p = key.UTF8String;
    if (!p || strlen(p) != key.length) return nil;
    if (MDHostFingerprint(key)) return key;
    uint8_t bytes[32];
    if (![key hasPrefix:@"npub1"] || nostr_nip19_decode_npub(p, bytes)) return nil;
    char hex[65];
    for (size_t i = 0; i < sizeof(bytes); ++i) snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
    return @(hex);
}

static BOOL PublicLabel(NSString *label) {
    if (label.length >= sizeof(((MdAccount *)0)->label)) return NO;
    NSString *trim = MDHostTrim(label);
    if (MDClassifyHostInput(trim) == MDHostSecret || [trim.lowercaseString hasPrefix:@"bunker://"] ||
        [trim.lowercaseString hasPrefix:@"nostrconnect://"] || [trim containsString:@"client_secret"] ||
        MDHostFingerprint(trim)) return NO;
    const char *p = label.UTF8String;
    return p && strlen(p) == [label lengthOfBytesUsingEncoding:NSUTF8StringEncoding] &&
        strlen(p) < sizeof(((MdAccount *)0)->label) && [label isEqual:MDHostBoundText(label,128)] &&
        [label rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location == NSNotFound;
}

static NSString *SaveMetadata(NSString *directory, MdSignerType backend, NSString *key, NSString *label, BOOL add) {
    if (!key) return @"Enter an npub or lowercase public key, never a secret key.";
    if (!PublicLabel(label)) return @"Enter a short public label (up to 128 UTF-8 bytes), not credentials.";
    MdAccounts *a = NULL;
    int rc = md_accounts_open(directory.fileSystemRepresentation, NULL, &a);
    if (!rc) rc = add ? md_accounts_add(a, backend, key.UTF8String, label.UTF8String) :
                         md_accounts_label(a, backend, key.UTF8String, label.UTF8String);
    if (!rc) rc = md_accounts_save(a);
    md_accounts_destroy(a);
    return rc ? MDIdentityError(rc,backend) : @"";
}
NSString *MDIdentityAdd(NSString *directory, MdSignerType backend, NSString *publicKey, NSString *label) {
    if (!MDIdentitySupportsBackend(backend)) return @"Choose a supported external signer backend.";
    return SaveMetadata(directory, backend, PublicKey(publicKey), label, YES);
}
NSString *MDIdentityLabel(NSString *directory, NSDictionary *account, NSString *label) {
    if (!account) return @"Choose a saved signer account.";
    return SaveMetadata(directory, [account[@"backend"] intValue], PublicKey(account[@"key"]), label, NO);
}
NSString *MDIdentityImportPairing(NSString *directory, MdCredentialStore *store, NSDictionary *account,
    const char *json, size_t length, MDIdentityCancelled cancelled, void *context) {
    NSString *key = PublicKey(account[@"key"]);
    if (!key || [account[@"backend"] intValue] != MD_SIGNER_NIP46 || !MDIdentitySupportsBackend(MD_SIGNER_NIP46))
        return @"Choose a saved NIP-46 signer account.";
    if (!json || length < 2 || length > MDIdentityPairingMax + 1 || memchr(json, '\0', length) != json + length - 1)
        return @"Enter valid saved NIP-46 pairing JSON.";
    MdNip46Credentials *credential = NULL;
    int rc = md_nip46_credentials_parse(json, &credential);
    if (rc == MD_CREDENTIAL_INVALID) return @"Enter valid saved NIP-46 pairing JSON.";
    if (!rc && strcmp(credential->user_pubkey,key.UTF8String)) {
        md_nip46_credentials_free(credential);
        return @"The pairing is for a different public key. Nothing was imported.";
    }
    MdAccounts *a = NULL;
    if (!rc) rc = md_accounts_open(directory.fileSystemRepresentation, NULL, &a);
    if (!rc) {
        BOOL exists = NO;
        for (size_t i = 0; i < md_accounts_count(a); ++i) {
            const MdAccount *item = md_accounts_at(a,i);
            if (item->backend == MD_SIGNER_NIP46 && !strcmp(item->pubkey,key.UTF8String)) { exists = YES; break; }
        }
        if (!exists) rc = MD_ACCOUNTS_NOT_FOUND;
    }
    if (!rc && cancelled && cancelled(context)) rc = MD_SIGNER_ERR_CANCELLED;
    if (!rc) rc = md_credential_store_put(store,credential);
    md_accounts_destroy(a);
    md_nip46_credentials_free(credential);
    return rc ? MDIdentityError(rc,MD_SIGNER_NIP46) : @"";
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
