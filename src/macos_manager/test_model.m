#import "model.h"
#include "md_test.h"
#include <cjson/cJSON.h>

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

int main(void) {
    @autoreleasepool {
        check_permissions();
        check_keys();
    }
    return 0;
}
