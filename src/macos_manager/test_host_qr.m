#import "host_qr.h"
#import "hosts_model.h"
#import <ImageIO/ImageIO.h>
#include "md_test.h"
#include <nostr/nip19/nip19.h>
#include <unistd.h>

#define CHECK(x) do { MD_CHECK(x); if (md_test_failures) return md_test_report(); } while (0)

static BOOL WriteQR(NSString *value, NSURL *file) {
    CGImageRef image = MDCreateHostQR(value);
    if (!image) return NO;
    CGImageDestinationRef png = CGImageDestinationCreateWithURL((__bridge CFURLRef)file, CFSTR("public.png"), 1, NULL);
    BOOL ok = NO;
    if (png) {
        CGImageDestinationAddImage(png, image, NULL);
        ok = CGImageDestinationFinalize(png);
        CFRelease(png);
    }
    CGImageRelease(image);
    return ok;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        CHECK(argc == 2);
        NSString *directory = @(argv[1]);
        CHECK(!MDSavedHostProfile(directory, NULL));
        MdAccounts *a = NULL;
        MdSigner *fixture = md_signer_create_direct("0000000000000000000000000000000000000000000000000000000000000001");
        char *key = NULL;
        CHECK(fixture && !md_signer_get_pubkey(fixture, &key));
        CHECK(!md_accounts_open(directory.fileSystemRepresentation, NULL, &a));
        CHECK(!md_accounts_add(a, MD_SIGNER_DIRECT_KEY, key, "Fixture only"));
        CHECK(!md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, key, fixture));
        CHECK(!md_accounts_save(a));
        md_accounts_destroy(a);
        md_signer_destroy(fixture);
        NSString *fingerprint = nil;
        NSString *profile = MDSavedHostProfile(directory, &fingerprint);
        CHECK([fingerprint isEqual:MDHostFingerprint(@(key))]);
        NostrProfilePointer *decoded = NULL;
        CHECK(profile.length && !nostr_nip19_decode_nprofile(profile.UTF8String, &decoded) && decoded);
        CHECK(!strcmp(key, decoded->public_key) && decoded->relays_count == 0);
        nostr_profile_pointer_free(decoded);
        free(key);
        NSURL *root = [NSURL fileURLWithPath:directory isDirectory:YES];
        NSURL *file = [root URLByAppendingPathComponent:@"profile.png"];
        CHECK(WriteQR(profile, file));
        NSString *scan = MDReadHostQR(file);
        CHECK([scan isEqual:profile] && MDClassifyHostInput(scan) == MDHostPublic);
        NSURL *secret = [root URLByAppendingPathComponent:@"secret-marker.png"];
        CHECK(WriteQR(@"nostr:nsec1fixture-not-a-key", secret));
        CHECK(MDClassifyHostInput(MDReadHostQR(secret)) == MDHostSecret);
        NSURL *link = [root URLByAppendingPathComponent:@"link.png"];
        CHECK(!symlink(file.fileSystemRepresentation, link.fileSystemRepresentation));
        CHECK(!MDReadHostQR(link));
        CHECK(!MDReadHostQR(root));
        NSURL *invalid = [root URLByAppendingPathComponent:@"invalid.png"];
        CHECK([@"not an image" writeToURL:invalid atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
        CHECK(!MDReadHostQR(invalid));
        CHECK(!truncate(invalid.fileSystemRepresentation, 16 * 1024 * 1024 + 1));
        CHECK(!MDReadHostQR(invalid));
        return md_test_report();
    }
}
