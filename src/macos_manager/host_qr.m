#import "host_qr.h"
#import "hosts_model.h"
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>
#import <Vision/Vision.h>
#include <errno.h>
#include <fcntl.h>
#include <nostr/nip19/nip19.h>
#include <sys/stat.h>
#include <unistd.h>

NSString *MDSavedHostProfile(NSString *directory, NSString **fingerprint) {
    if (fingerprint) *fingerprint = nil;
    MdAccounts *accounts = NULL;
    if (md_accounts_open_snapshot(directory.fileSystemRepresentation, NULL, &accounts)) return nil;
    const MdAccount *host = md_accounts_role(accounts, MD_ACCOUNT_HOST);
    NSString *fp = host ? MDHostFingerprint(@(host->pubkey)) : nil;
    char *raw = NULL;
    NSString *profile = nil;
    if (fp) {
        NostrProfilePointer ptr = {.public_key = (char *)host->pubkey};
        if (!nostr_nip19_encode_nprofile(&ptr, &raw) && raw) profile = @(raw);
    }
    free(raw);
    md_accounts_destroy(accounts);
    if (profile && fingerprint) *fingerprint = fp;
    return profile;
}

CGImageRef MDCreateHostQR(NSString *profile) {
    if (!profile.length || profile.length > MD_MANAGED_ADDRESS_MAX) return NULL;
    CIFilter *filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
    [filter setValue:[profile dataUsingEncoding:NSUTF8StringEncoding] forKey:@"inputMessage"];
    [filter setValue:@"M" forKey:@"inputCorrectionLevel"];
    CIImage *code = filter.outputImage;
    if (!code || CGRectIsEmpty(code.extent)) return NULL;
    CIContext *renderer = [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer:@YES}];
    CGImageRef modules = [renderer createCGImage:code fromRect:code.extent];
    if (!modules) return NULL;
    const size_t scale = 6, quiet = 4 * scale;
    size_t size = CGImageGetWidth(modules) * scale;
    size_t side = size + 2 * quiet;
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef bitmap = CGBitmapContextCreate(NULL, side, side, 8, 0, rgb, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(rgb);
    CGImageRef result = NULL;
    if (bitmap) {
        CGContextSetRGBFillColor(bitmap, 1, 1, 1, 1);
        CGContextFillRect(bitmap, CGRectMake(0, 0, side, side));
        CGContextSetInterpolationQuality(bitmap, kCGInterpolationNone);
        CGContextDrawImage(bitmap, CGRectMake(quiet, quiet, size, size), modules);
        result = CGBitmapContextCreateImage(bitmap);
        CGContextRelease(bitmap);
    }
    CGImageRelease(modules);
    return result;
}

static NSData *ReadImageFile(NSURL *url) {
    const size_t max = 16 * 1024 * 1024;
    if (!url.isFileURL) return nil;
    int fd = open(url.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return nil;
    struct stat st;
    NSMutableData *data = nil;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size <= max) {
        data = [NSMutableData dataWithLength:(NSUInteger)st.st_size];
        size_t used = 0;
        while (used < data.length) {
            ssize_t n = read(fd, (char *)data.mutableBytes + used, data.length - used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { data = nil; break; }
            used += (size_t)n;
        }
        unsigned char extra;
        ssize_t n;
        if (data) {
            do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
            if (n != 0) data = nil;
        }
    }
    close(fd);
    return data;
}

NSString *MDReadHostQR(NSURL *url) {
    NSData *data = ReadImageFile(url);
    if (!data) return nil;
    NSDictionary *options = @{(__bridge NSString *)kCGImageSourceShouldCache:@NO};
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, (__bridge CFDictionaryRef)options);
    if (!source) return nil;
    CGImageRef image = NULL;
    if (CGImageSourceGetCount(source) == 1) {
        NSDictionary *props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, NULL));
        uint64_t w = [props[(__bridge NSString *)kCGImagePropertyPixelWidth] unsignedLongLongValue];
        uint64_t h = [props[(__bridge NSString *)kCGImagePropertyPixelHeight] unsignedLongLongValue];
        if (w && h && w <= 8192 && h <= 8192 && w * h <= 32 * 1024 * 1024) {
            NSDictionary *thumbnail = @{(__bridge NSString *)kCGImageSourceShouldCache:@NO,
                (__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways:@YES,
                (__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize:@2048,
                (__bridge NSString *)kCGImageSourceCreateThumbnailWithTransform:@YES};
            image = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)thumbnail);
        }
    }
    CFRelease(source);
    if (!image) return nil;
    VNDetectBarcodesRequest *request = [VNDetectBarcodesRequest new];
    request.symbologies = @[VNBarcodeSymbologyQR];
    VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
    BOOL ok = [handler performRequests:@[request] error:NULL];
    CGImageRelease(image);
    NSString *value = ok && request.results.count == 1 ? request.results.firstObject.payloadStringValue : nil;
    if (value.length > MD_MANAGED_ADDRESS_MAX || [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > MD_MANAGED_ADDRESS_MAX) return nil;
    return value;
}
