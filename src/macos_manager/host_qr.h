#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

/* Worker-only. The saved selection is public metadata, not a live signer claim. */
NSString *MDSavedHostProfile(NSString *directory, NSString **fingerprint);
CGImageRef MDCreateHostQR(NSString *profile) CF_RETURNS_RETAINED;
/* Decode exactly one QR from a bounded raster file. The caller must classify and
 * review the returned value with the same policy as manually entered text. */
NSString *MDReadHostQR(NSURL *url);
