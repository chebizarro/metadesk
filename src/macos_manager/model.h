#import <Foundation/Foundation.h>

/* Foundation-only projections of the C core; no AppKit or TCC calls here. */
typedef NS_ENUM(NSInteger, MDPermission) { MDPermissionUnknown, MDPermissionMissing, MDPermissionGranted };
typedef NS_ENUM(NSInteger, MDPermissionKind) { MDScreenRecording, MDAccessibility };
MDPermission MDHostPermission(NSDictionary *snapshot, MDPermissionKind kind);
NSURL *MDPermissionSettingsURL(MDPermissionKind kind);
NSDictionary *MDHostRead(NSString *socketPath);
NSString *MDPublicIdentity(const char *hex);
NSString *MDAccountsSummary(void);
NSString *MDRoutePreview(NSString *address, NSString *relay);
