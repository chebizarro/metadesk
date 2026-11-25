#import "model.h"
#include "md_test.h"

#define KEY "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"

int main(void) {
    @autoreleasepool {
        MD_CHECK([MDPermissionSettingsURL(MDScreenRecording).absoluteString isEqualToString:
            @"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_ScreenCapture"]);
        MD_CHECK([MDPermissionSettingsURL(MDAccessibility).absoluteString isEqualToString:
            @"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_Accessibility"]);
        NSString *text=MDRoutePreview(@KEY,@"");
        MD_CHECK([text hasPrefix:@"npub1"] && [text containsString:@"79be 667e f9dc bbac"]);
        MD_CHECK([text containsString:@"No bootstrap relay selected"]);
        MD_CHECK([MDRoutePreview(@KEY,@"wss://EXAMPLE.COM/") containsString:@"wss://example.com"]);
        MD_CHECK([MDRoutePreview(@KEY,@"https://example.com") containsString:@"Invalid relay URL"]);
        MD_CHECK([MDRoutePreview(@"127.0.0.1",@"") containsString:@"not an identity"]);
        MD_CHECK([MDRoutePreview(@"name@example.com",@"") containsString:@"not part of this offline preview"]);
    }
    return md_test_report();
}
