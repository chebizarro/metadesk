#import "model.h"
#include "md_test.h"
#include <cjson/cJSON.h>
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
    MD_CHECK([MDRoutePreview(@KEY, @"") containsString:@"No bootstrap relay selected"]);
    MD_CHECK([MDRoutePreview(@KEY, @"wss://EXAMPLE.COM/") containsString:@"wss://example.com"]);
    MD_CHECK([MDRoutePreview(@KEY, @"https://example.com") containsString:@"Invalid relay URL"]);
    MD_CHECK([MDRoutePreview(@"127.0.0.1", @"") containsString:@"not an identity"]);
    MD_CHECK([MDRoutePreview(@"name@example.com", @"") containsString:@"not part of this offline preview"]);
}

/* Read real fixture metadata without restoring a signer. The replacement
 * Hosts model exercises encrypted cache and sync with a fixture binding. */
static void check_accounts(void) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *home = [files.temporaryDirectory.path stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSDictionary *directoryMode = @{NSFilePosixPermissions: @0700};
    BOOL created = [files createDirectoryAtPath:home withIntermediateDirectories:NO attributes:directoryMode error:nil];
    MD_CHECK(created);
    if (!created) return;
    // The account reader rejects symlinks in parent paths (including /var and /tmp).
    char *canonical = realpath(home.fileSystemRepresentation, NULL);
    MD_CHECK(canonical != NULL);
    if (!canonical) { MD_CHECK([files removeItemAtPath:home error:nil]); return; }
    NSString *accounts = [[files stringWithFileSystemRepresentation:canonical length:strlen(canonical)]
        stringByAppendingPathComponent:@"Library/Application Support/metadesk"];
    const char *saved = getenv("HOME");
    char *previousHome = saved ? strdup(saved) : NULL;
    int rc = setenv("HOME", canonical, 1);
    free(canonical);
    MD_CHECK(!rc);
    @try {
        if (rc) return; /* never read the operator's account directory on setup failure */
        created = [files createDirectoryAtPath:accounts withIntermediateDirectories:YES attributes:directoryMode error:nil];
        MD_CHECK(created);
        if (!created) return;
        NSString *emptySummary = MDAccountsSummary();
        MD_CHECK_MSG([emptySummary containsString:@"Client role\nNot assigned\n\nHost role\nNot assigned"], emptySummary.UTF8String);
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
        check_accounts();
    }
    return md_test_report();
}
