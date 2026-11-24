#import "model.h"
#include "md_test.h"
#include <cjson/cJSON.h>
#include "managed_store.h"
#include <stdlib.h>
#include <string.h>

/* Override the transport for this test executable: no real socket or TCC call. */
static const char *permission_reply;
static unsigned calls;
cJSON *md_management_call(const char *path, const cJSON *request) {
    MD_CHECK(!strcmp(path, "/fixture/control.sock"));
    MD_CHECK(cJSON_GetArraySize(request) == 2);
    MD_CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(request, "v")) == 1);
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "method"));
    MD_CHECK(method && !strcmp(method, calls++ ? "management.snapshot" : "host.permissions"));
    const char *reply = calls == 1 ? permission_reply : "{\"v\":1,\"ok\":true,\"session\":null}";
    return reply ? cJSON_Parse(reply) : NULL;
}

static void check_permissions(void) {
    NSDictionary *snapshot = @{@"v": @1, @"ok": @YES,
        @"permissions": @{@"screen_recording": @YES, @"accessibility": @NO}};
    MD_CHECK(MDHostPermission(snapshot, MDScreenRecording) == MDPermissionGranted);
    MD_CHECK(MDHostPermission(snapshot, MDAccessibility) == MDPermissionMissing);
    NSDictionary *recheck = @{@"v": @1, @"ok": @YES,
        @"permissions": @{@"screen_recording": @NO, @"accessibility": @YES}};
    MD_CHECK(MDHostPermission(recheck, MDScreenRecording) == MDPermissionMissing);
    MD_CHECK(MDHostPermission(recheck, MDAccessibility) == MDPermissionGranted);
    for (NSDictionary *bad in @[
         @{@"v": @2, @"ok": @YES, @"permissions": snapshot[@"permissions"]},
         @{@"v": @1, @"ok": @1, @"permissions": snapshot[@"permissions"]},
         @{@"v": @1, @"ok": @NO, @"permissions": snapshot[@"permissions"]},
         @{@"v": @1, @"ok": @YES, @"permissions": @[@YES]},
         @{@"v": @1, @"ok": @YES, @"permissions": @{@"screen_recording": @1, @"accessibility": @"true"}},
         @{@"v": @1, @"ok": @YES}
    ]) {
        MD_CHECK(MDHostPermission(bad, MDScreenRecording) == MDPermissionUnknown);
        MD_CHECK(MDHostPermission(bad, MDAccessibility) == MDPermissionUnknown);
    }
    MD_CHECK(MDHostPermission(nil, MDAccessibility) == MDPermissionUnknown);
    MD_CHECK([MDPermissionSettingsURL(MDScreenRecording).absoluteString isEqualToString:
        @"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_ScreenCapture"]);
    MD_CHECK([MDPermissionSettingsURL(MDAccessibility).absoluteString isEqualToString:
        @"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_Accessibility"]);
    MD_CHECK(!MDHostRead(@"relative.sock") && calls == 0);
    permission_reply = "{\"v\":1,\"ok\":true,\"permissions\":{\"screen_recording\":true}}";
    MD_CHECK(MDHostPermission(MDHostRead(@"/fixture/control.sock"), MDScreenRecording) == MDPermissionGranted && calls == 1);
    calls = 0;
    permission_reply = "{\"v\":1,\"ok\":false,\"error\":\"invalid\"}";
    NSDictionary *legacy = MDHostRead(@"/fixture/control.sock");
    MD_CHECK(legacy && calls == 2 && MDHostPermission(legacy, MDScreenRecording) == MDPermissionUnknown);
    calls = 0;
    permission_reply = NULL;
    MD_CHECK(!MDHostRead(@"/fixture/control.sock") && calls == 1);
}

#define KEY "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
static void check_keys(void) {
    NSString *key = MDPublicIdentity(KEY);
    MD_CHECK([key hasPrefix:@"npub1"] && [key hasSuffix:@"79be 667e f9dc bbac"]);
    MD_CHECK(!MDPublicIdentity("invalid"));
    MD_CHECK([MDFormatCachedHosts("[[\"extension\",\"value\"],[\"p\",\"" KEY "\"]]") isEqualToString:key]);
    MD_CHECK([MDFormatCachedHosts("[]") containsString:@"empty"]);
    MD_CHECK([MDFormatCachedHosts(NULL) containsString:@"No committed"]);
    MD_CHECK([MDFormatCachedHosts("not JSON") containsString:@"unavailable"]);
    MD_CHECK([MDFormatCachedHosts("[[\"p\",\"no key\"]]") containsString:@"unavailable"]);
    MD_CHECK([MDCachedHosts(@"relative/path") containsString:@"absolute"]);
    MD_CHECK([MDRoutePreview(@KEY, @"") containsString:@"No bootstrap relay selected"]);
    MD_CHECK([MDRoutePreview(@KEY, @"wss://EXAMPLE.COM/") containsString:@"wss://example.com"]);
    MD_CHECK([MDRoutePreview(@KEY, @"https://example.com") containsString:@"Invalid relay URL"]);
    MD_CHECK([MDRoutePreview(@"127.0.0.1", @"") containsString:@"not an identity"]);
    MD_CHECK([MDRoutePreview(@"name@example.com", @"") containsString:@"not part of this offline preview"]);
}

/* Real saved metadata snapshot, fake signer restoration and encrypted store.
 * Opaque tokens must never reach a real signer or store function. */
static char binding_token, store_token;
static int startup_error;
static MdStoreResult store_status;
static const char *store_winner;
static unsigned store_opens, store_closes;

int md_account_startup_open(MdAccountRole role, MdAccountStartup *startup) {
    MD_CHECK(role == MD_ACCOUNT_CLIENT);
    if (startup_error) return startup_error;
    int rc = md_accounts_open_snapshot(NULL, NULL, &startup->accounts);
    if (!rc && md_accounts_role(startup->accounts, role))
        startup->binding = (MdAccountBinding *)&binding_token;
    return rc;
}

void md_account_startup_clear(MdAccountStartup *startup) {
    MD_CHECK(!startup->binding || startup->binding == (MdAccountBinding *)&binding_token);
    MD_CHECK(store_opens == store_closes);
    md_accounts_destroy(startup->accounts);
}

MdManagedStore *md_managed_store_open(MdAccountBinding *binding, const char *directory) {
    MD_CHECK(binding == (MdAccountBinding *)&binding_token);
    MD_CHECK(directory && directory[0] == '/');
    ++store_opens;
    return (MdManagedStore *)&store_token;
}

MdStoreResult md_managed_store_read(MdManagedStore *store, const char *coordinate,
                                   char revision[65], char **winner, char **pending) {
    MD_CHECK(store == (MdManagedStore *)&store_token && !strcmp(coordinate, MD_STORE_HOSTS));
    MD_CHECK(!revision && winner && !pending);
    if (winner) *winner = store_winner ? strdup(store_winner) : NULL;
    return store_status;
}

void md_managed_store_close(MdManagedStore *store) {
    MD_CHECK(store == (MdManagedStore *)&store_token);
    ++store_closes;
}

static void check_accounts_and_cache(void) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *home = [files.temporaryDirectory.path stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSString *accounts = [home stringByAppendingPathComponent:@"Library/Application Support/metadesk"];
    NSDictionary *directoryMode = @{NSFilePosixPermissions: @0700};
    BOOL created = [files createDirectoryAtPath:home withIntermediateDirectories:NO attributes:directoryMode error:nil];
    MD_CHECK(created);
    if (!created) return;
    const char *saved = getenv("HOME");
    char *previousHome = saved ? strdup(saved) : NULL;
    int rc = setenv("HOME", home.fileSystemRepresentation, 1);
    MD_CHECK(!rc);
    @try {
        if (rc) return; /* never read the operator's account directory on setup failure */
        created = [files createDirectoryAtPath:accounts withIntermediateDirectories:YES attributes:directoryMode error:nil];
        MD_CHECK(created);
        if (!created) return;
        MD_CHECK([MDAccountsSummary() containsString:@"Client role\nNot assigned\n\nHost role\nNot assigned"]);
        MD_CHECK([MDCachedHosts(home) containsString:@"Assign a client role"] && store_opens == 0);
        NSString *pair = @"{\"backend\":\"nip46\",\"pubkey\":\"" KEY "\"}";
        NSString *metadata = [NSString stringWithFormat:
            @"{\"version\":1,\"accounts\":[{\"backend\":\"nip46\",\"pubkey\":\"" KEY "\",\"label\":\"Fixture Account\"}],"
             "\"roles\":{\"client\":%@,\"host\":null}}", pair];
        created = [files createFileAtPath:[accounts stringByAppendingPathComponent:@"management.json"]
            contents:[metadata dataUsingEncoding:NSUTF8StringEncoding] attributes:@{NSFilePosixPermissions: @0600}];
        MD_CHECK(created);
        if (!created) return;
        NSString *summary = MDAccountsSummary();
        MD_CHECK([summary containsString:@"Client role\nFixture Account · nip46\nnpub1"]);
        MD_CHECK([summary containsString:@"79be 667e f9dc bbac\n\nHost role\nNot assigned"]);
        startup_error = MD_CREDENTIAL_LOCKED;
        NSString *locked = MDCachedHosts(home);
        MD_CHECK([locked containsString:@"Keychain"] && [locked containsString:@"no external signer was reached"]);
        MD_CHECK(store_opens == 0);
        startup_error = 0;

        store_status = MD_STORE_OK;
        MD_CHECK([MDCachedHosts(home) containsString:@"No committed Hosts list"]);
        store_winner = "[[\"p\",\"" KEY "\"]]";
        const MdStoreResult statuses[] = {MD_STORE_PENDING, MD_STORE_PARTIAL, MD_STORE_CONFLICT};
        NSArray<NSString *> *draftMessages = @[@"pending publication", @"partially acknowledged", @"conflicts"];
        for (size_t i = 0; i < sizeof(statuses)/sizeof(*statuses); i++) {
            store_status = statuses[i];
            NSString *message = MDCachedHosts(home);
            MD_CHECK([message containsString:draftMessages[i]] && [message containsString:MDPublicIdentity(KEY)]);
        }
        store_winner = NULL;
        const MdStoreResult errors[] = {MD_STORE_READ_ONLY, MD_STORE_IO, MD_STORE_REVOKED};
        NSArray<NSString *> *errorMessages = @[@"unreadable", @"from disk", @"no longer current"];
        for (size_t i = 0; i < sizeof(errors)/sizeof(*errors); i++) {
            store_status = errors[i];
            NSString *message = MDCachedHosts(home);
            MD_CHECK([message containsString:errorMessages[i]] && ![message containsString:@"empty"] &&
                     ![message containsString:@"No committed"]);
        }
        MD_CHECK(store_opens == 7 && store_closes == store_opens);
    } @finally {
        MD_CHECK(!(previousHome ? setenv("HOME", previousHome, 1) : unsetenv("HOME")));
        free(previousHome);
        MD_CHECK([files removeItemAtPath:home error:nil]);
    }
}

int main(void) {
    @autoreleasepool {
        check_permissions();
        check_keys();
        check_accounts_and_cache();
    }
    return md_test_report();
}
