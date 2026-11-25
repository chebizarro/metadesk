#import "host_qr.h"
#import "hosts_model.h"
#import <ImageIO/ImageIO.h>
#include "md_test.h"
#include <nostr/nip19/nip19.h>
#include <unistd.h>

#define CHECK(x) do { MD_CHECK(x); if (md_test_failures) return md_test_report(); } while (0)

static BOOL WriteImage(CGImageRef image, NSURL *file) {
    if (!image) return NO;
    CGImageDestinationRef png = CGImageDestinationCreateWithURL((__bridge CFURLRef)file, CFSTR("public.png"), 1, NULL);
    BOOL ok = NO;
    if (png) {
        CGImageDestinationAddImage(png, image, NULL);
        ok = CGImageDestinationFinalize(png);
        CFRelease(png);
    }
    return ok;
}
static BOOL WriteQR(NSString *value, NSURL *file) {
    CGImageRef image = MDCreateHostQR(value);
    BOOL ok = WriteImage(image,file);
    if (image) CGImageRelease(image);
    return ok;
}
static BOOL WriteCanvas(NSString *value, BOOL two, size_t width, size_t height, NSURL *file) {
    CGImageRef qr = MDCreateHostQR(value);
    if (!qr) return NO;
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef canvas = CGBitmapContextCreate(NULL,width,height,8,0,gray,(CGBitmapInfo)kCGImageAlphaNone);
    BOOL ok = NO;
    if (canvas) {
        CGContextSetGrayFillColor(canvas,1,1);
        CGContextFillRect(canvas,CGRectMake(0,0,width,height));
        CGFloat size=CGImageGetWidth(qr);
        CGContextDrawImage(canvas,CGRectMake(0,0,size,size),qr);
        if (two) {
            CGImageRef other=MDCreateHostQR(@"desk@example.invalid");
            if (other) {
                CGContextDrawImage(canvas,CGRectMake(width-size,0,size,size),other);
                CGImageRelease(other);
            }
        }
        CGImageRef image=CGBitmapContextCreateImage(canvas);
        ok=WriteImage(image,file);
        if (image) CGImageRelease(image);
        CGContextRelease(canvas);
    }
    CGColorSpaceRelease(gray);
    CGImageRelease(qr);
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
        CHECK([MDHostQRProfile([@"nostr:" stringByAppendingString:scan]) isEqual:profile]);
        CHECK(!MDHostQRProfile(@"desk@example.invalid") && !MDHostQRProfile(@"nprofile1invalid") &&
              !MDHostQRProfile(MDHostNpub(@"79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798")));
        NSURL *multi=[root URLByAppendingPathComponent:@"multi.png"];
        CHECK(WriteCanvas(profile,YES,800,300,multi) && !MDReadHostQR(multi));
        CHECK(WriteCanvas(profile,NO,800,300,multi) && [MDReadHostQR(multi) isEqual:profile]);
        CHECK(WriteCanvas(profile,NO,8193,300,multi) && !MDReadHostQR(multi));
        CHECK(WriteCanvas(profile,NO,6000,6000,multi) && !MDReadHostQR(multi));
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
