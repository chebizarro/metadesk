#import "model.h"
#include "identity_resolver.h"
#include "relay_plan.h"

NSURL *MDPermissionSettingsURL(MDPermissionKind kind) {
    NSString *anchor = kind == MDScreenRecording ? @"Privacy_ScreenCapture" : @"Privacy_Accessibility";
    return [NSURL URLWithString:[@"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?"
                                 stringByAppendingString:anchor]];
}

NSString *MDRoutePreview(NSString *address, NSString *relay) {
    if ([address containsString:@"@"]) return @"Live NIP-05 lookup is not part of this offline preview.";
    if (address.length > 8192 || relay.length > MD_RELAY_URL_MAX) return @"Identity or relay URL is too long.";
    MdIdentity identity = {0};
    MdResolveResult resolved = md_identity_resolve(address.UTF8String, NULL, &identity);
    if (resolved != MD_RESOLVE_OK) return @"Enter a valid npub, nprofile or hex public key. An IP address is not an identity.";
    char fp[MD_KEY_FINGERPRINT_SIZE];
    char *npub=md_identity_npub(&identity);
    NSString *key = npub && md_key_fingerprint(identity.pubkey,fp) ? [NSString stringWithFormat:@"%s\n%s",npub,fp] : nil;
    free(npub); md_identity_clear(&identity);
    if (!key) return @"Public key unavailable.";
    MdRelayPlan bootstrap = {0}, plan = {0};
    NSString *route;
    if (relay.length && !md_relay_plan_add(&bootstrap, relay.UTF8String, MD_RELAY_CONFIGURED))
        route = @"Invalid relay URL. Enter an explicit ws:// or wss:// URL.";
    else if (md_relay_plan_build(&plan, MD_RELAY_BOOTSTRAP, &bootstrap, NULL, NULL) != MD_RELAY_READY)
        route = @"No bootstrap relay selected. No default relay will be used.";
    else
        route = [NSString stringWithFormat:@"Bootstrap preview: %s\nNo relay contacted; this is not a delivery plan.", plan.target[0].url];
    md_relay_plan_clear(&plan); md_relay_plan_clear(&bootstrap);
    return [NSString stringWithFormat:@"%@\n\n%@", key, route];
}
