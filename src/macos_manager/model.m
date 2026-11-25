#import "model.h"

#include "accounts.h"
#include "identity_resolver.h"
#include "managed_store.h"
#include "management_ipc.h"
#include "relay_plan.h"
#include <openssl/crypto.h>
#include <string.h>

static NSDictionary *Object(id value) {
    return [value isKindOfClass:NSDictionary.class] ? value : nil;
}

static BOOL IsBoolean(id value) {
    return [value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID();
}

static BOOL VersionedReply(NSDictionary *snapshot) {
    id version = snapshot[@"v"];
    return [version isKindOfClass:NSNumber.class] && !IsBoolean(version) &&
           [version isEqualToNumber:@1] && IsBoolean(snapshot[@"ok"]);
}

static BOOL ValidReply(NSDictionary *snapshot) {
    return VersionedReply(snapshot) && [snapshot[@"ok"] boolValue];
}

MDPermission MDHostPermission(NSDictionary *snapshot, MDPermissionKind kind) {
    if (!ValidReply(snapshot)) return MDPermissionUnknown;
    id value = Object(snapshot[@"permissions"])[kind == MDScreenRecording ? @"screen_recording" : @"accessibility"];
    if (!IsBoolean(value)) return MDPermissionUnknown;
    return [value boolValue] ? MDPermissionGranted : MDPermissionMissing;
}

NSURL *MDPermissionSettingsURL(MDPermissionKind kind) {
    NSString *anchor = kind == MDScreenRecording ? @"Privacy_ScreenCapture" : @"Privacy_Accessibility";
    return [NSURL URLWithString:[@"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?"
                                 stringByAppendingString:anchor]];
}

static NSDictionary *HostCall(NSString *socketPath, const char *method) {
    cJSON *request = cJSON_CreateObject();
    cJSON *reply = NULL;
    if (cJSON_AddNumberToObject(request, "v", 1) &&
        cJSON_AddStringToObject(request, "method", method))
        reply = md_management_call(socketPath.fileSystemRepresentation, request);
    cJSON_Delete(request);
    char *wire = reply ? cJSON_PrintUnformatted(reply) : NULL;
    cJSON_Delete(reply);
    NSDictionary *snapshot = wire ? Object([NSJSONSerialization JSONObjectWithData:
        [NSData dataWithBytes:wire length:strlen(wire)] options:0 error:nil]) : nil;
    cJSON_free(wire);
    return snapshot;
}

NSDictionary *MDHostRead(NSString *socketPath) {
    if (![socketPath hasPrefix:@"/"]) return nil;
    NSDictionary *snapshot = HostCall(socketPath, "host.permissions");
    /* A v1 host predating the method returns invalid. Do not retry a failed
     * or unauthenticated connection, or interpret the legacy reply as TCC state. */
    if (VersionedReply(snapshot) && ![snapshot[@"ok"] boolValue] && [snapshot[@"error"] isEqual:@"invalid"])
        snapshot = HostCall(socketPath, "management.snapshot");
    return ValidReply(snapshot) ? snapshot : nil;
}

static NSString *Text(const char *text) {
    return text ? [NSString stringWithUTF8String:text] : nil;
}

NSString *MDPublicIdentity(const char *hex) {
    if (!hex || strlen(hex) != 64 || strspn(hex, "0123456789abcdef") != 64) return nil;
    MdIdentity id = {0};
    memcpy(id.pubkey, hex, sizeof(id.pubkey));
    char *npub = md_identity_npub(&id);
    NSString *value = Text(npub);
    free(npub);
    return value ? [NSString stringWithFormat:@"%@\n%.4s %.4s %.4s %.4s", value, hex, hex+4, hex+8, hex+12] : nil;
}

static NSString *Account(const MdAccount *account) {
    if (!account) return @"Not assigned";
    return [NSString stringWithFormat:@"%@ · %@\n%@", Text(account->label).length ? Text(account->label) : @"Unnamed account",
            Text(md_accounts_backend_name(account->backend)), MDPublicIdentity(account->pubkey) ?: @"Invalid public key"];
}

NSString *MDAccountsSummary(void) {
    MdAccounts *accounts = NULL;
    int rc = md_accounts_open_snapshot(NULL, NULL, &accounts);
    if (rc) return [NSString stringWithFormat:@"Accounts unavailable: %s", md_accounts_error_string(rc)];
    NSMutableString *summary = [NSMutableString stringWithFormat:@"Client role\n%@\n\nHost role\n%@\n\nAccounts",
        Account(md_accounts_role(accounts, MD_ACCOUNT_CLIENT)), Account(md_accounts_role(accounts, MD_ACCOUNT_HOST))];
    for (size_t i = 0; i < md_accounts_count(accounts); i++)
        [summary appendFormat:@"\n\n%@", Account(md_accounts_at(accounts, i))];
    md_accounts_destroy(accounts);
    return summary;
}

NSString *MDFormatCachedHosts(const char *winner) {
    if (!winner) return @"No committed Hosts list in this cache. Live history was not checked.";
    id tags = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytesNoCopy:(void *)winner
        length:strlen(winner) freeWhenDone:NO] options:0 error:nil];
    if (![tags isKindOfClass:NSArray.class]) return @"Cached Hosts list unavailable.";
    NSMutableArray<NSString *> *rows = [NSMutableArray array];
    for (id tag in tags) {
        if (![tag isKindOfClass:NSArray.class] || ![tag count] || ![tag[0] isKindOfClass:NSString.class])
            return @"Cached Hosts list unavailable.";
        if (![tag[0] isEqual:@"p"]) continue;
        if ([tag count] != 2 || ![tag[1] isKindOfClass:NSString.class]) return @"Cached Hosts list unavailable.";
        NSString *key = MDPublicIdentity([tag[1] UTF8String]);
        if (!key) return @"Cached Hosts list unavailable.";
        [rows addObject:key];
    }
    return rows.count ? [rows componentsJoinedByString:@"\n\n"] : @"The cached Hosts list is empty.";
}

NSString *MDCachedHosts(NSString *stateDirectory) {
    if (!stateDirectory.length) {
        char *path = md_managed_store_default_directory();
        stateDirectory = path ? [NSString stringWithUTF8String:path] : nil;
        free(path);
    }
    if (![stateDirectory hasPrefix:@"/"]) return @"Enter an absolute path to an existing private state directory.";
    MdAccountStartup startup = {0};
    int rc = md_account_startup_open(MD_ACCOUNT_CLIENT, &startup);
    NSString *result;
    if (rc) {
        switch (rc) {
        case MD_CREDENTIAL_NOT_FOUND: case MD_CREDENTIAL_LOCKED: case MD_CREDENTIAL_UNAVAILABLE:
        case MD_CREDENTIAL_INVALID: case MD_CREDENTIAL_FAILED: case MD_CREDENTIAL_AMBIGUOUS:
            result = [NSString stringWithFormat:@"Local NIP-46 credential access failed: %s. "
                "Check this build’s Keychain access and saved pairing; no external signer was reached.",
                md_credential_error_string(rc)];
            break;
        default:
            result = [NSString stringWithFormat:@"Could not load the client role: %s\n"
                "If the signer is unavailable, check it separately; Metadesk has no password.", md_accounts_error_string(rc)];
            break;
        }
    }
    else if (!startup.binding) result = @"Assign a client role with the account CLI before reading its cache.";
    else {
        MdManagedStore *store = md_managed_store_open(startup.binding, stateDirectory.fileSystemRepresentation);
        if (!store) result = @"Cache unavailable. Check the private directory and close other readers or writers for this account.";
        else {
            char *winner = NULL;
            MdStoreResult status = md_managed_store_read(store, MD_STORE_HOSTS, NULL, &winner, NULL);
            switch (status) {
            case MD_STORE_OK:
                result = MDFormatCachedHosts(winner);
                break;
            case MD_STORE_PENDING:
                result = [@"A local draft is pending publication. Showing only the committed cache.\n\n"
                    stringByAppendingString:MDFormatCachedHosts(winner)];
                break;
            case MD_STORE_PARTIAL:
                result = [@"Publication is only partially acknowledged. Showing the committed cache.\n\n"
                    stringByAppendingString:MDFormatCachedHosts(winner)];
                break;
            case MD_STORE_CONFLICT:
                result = [@"A local draft conflicts with the cached winner. Showing only the committed cache.\n\n"
                    stringByAppendingString:MDFormatCachedHosts(winner)];
                break;
            case MD_STORE_READ_ONLY:
                result = @"The cached Hosts list is unreadable or uses an unsupported schema.";
                break;
            case MD_STORE_IO:
                result = @"Could not read the cached Hosts list from disk.";
                break;
            case MD_STORE_REVOKED:
                result = @"The client role is no longer current. Refresh the saved roles before reading the cache again.";
                break;
            case MD_STORE_SIGNER:
                result = @"The external signer could not decrypt the cache. Check the signer and try again.";
                break;
            default:
                result = @"The cached Hosts list is unreadable; it was not treated as empty.";
                break;
            }
            if (winner) { OPENSSL_cleanse(winner, strlen(winner)); free(winner); }
            md_managed_store_close(store);
        }
    }
    md_account_startup_clear(&startup);
    return result;
}

NSString *MDRoutePreview(NSString *address, NSString *relay) {
    /* Deliberately offline: no NIP-05 fetch, hint DNS checks or relay dial. */
    if ([address containsString:@"@"]) return @"Live NIP-05 lookup is not part of this offline preview.";
    MdIdentity identity = {0};
    MdResolveResult resolved = md_identity_resolve(address.UTF8String, NULL, &identity);
    if (resolved != MD_RESOLVE_OK) return @"Enter a valid npub, nprofile or hex public key. An IP address is not an identity.";
    NSString *key = MDPublicIdentity(identity.pubkey);
    md_identity_clear(&identity);
    if (!key) return @"Public key unavailable.";

    MdRelayPlan bootstrap = {0}, plan = {0};
    NSString *route;
    if (relay.length && !md_relay_plan_add(&bootstrap, relay.UTF8String, MD_RELAY_CONFIGURED))
        route = @"Invalid relay URL. Enter an explicit ws:// or wss:// URL.";
    else if (md_relay_plan_build(&plan, MD_RELAY_BOOTSTRAP, &bootstrap, NULL, NULL) != MD_RELAY_READY)
        route = @"No bootstrap relay selected. No default relay will be used.";
    else
        route = [NSString stringWithFormat:@"Bootstrap preview: %s\nNo relay contacted; this is not a delivery plan.", plan.target[0].url];
    md_relay_plan_clear(&plan);
    md_relay_plan_clear(&bootstrap);
    return [NSString stringWithFormat:@"%@\n\n%@", key, route];
}
