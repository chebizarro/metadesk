#import <Foundation/Foundation.h>

/* Offline bootstrap preview and links; no permission or keychain calls. */
typedef NS_ENUM(NSInteger, MDPermissionKind) { MDScreenRecording, MDAccessibility };
NSURL *MDPermissionSettingsURL(MDPermissionKind kind);
NSString *MDRoutePreview(NSString *address, NSString *relay);
