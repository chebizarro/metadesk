#import <AppKit/AppKit.h>
#import "access_model.h"

@interface MDLocalHostPages : NSObject
- (instancetype)initWithLinkFactory:(MDHostLink *(^)(MDHostUpdate update))factory;
@property(nonatomic, readonly) NSView *computerView, *accessView;
/* Called on the main queue when the number of live pending requests changes. */
@property(nonatomic, copy) void (^pendingChanged)(NSUInteger count);
- (void)close:(dispatch_block_t)drained;
/* View > Refresh: This Computer rechecks permissions; Access reloads remembered clients. */
- (BOOL)canRefreshSection:(NSString *)section;
/* First shown: connect to the saved host role's standard socket. */
- (void)activate;
/* Identity reloaded its roles; key is the host role's key ("" when none,
 * nil when unknown). On a change the old link, its requests, remembered
 * clients and any open decision sheet are invalidated before the standard
 * socket is looked up again. A custom socket stays (it works with or without
 * a host role) until the field is cleared and Connect looks up afresh. */
- (void)hostRoleChanged:(NSString *)key;
/* Discovery seam (tests, renderer). Runs off the main queue; returns the
 * standard socket path and sets *hostKey to the host role's key (nil when
 * there is none). Default reads the saved host role. */
@property(nonatomic, copy) NSString *(^discover)(NSString **hostKey);
/* Opens the review for exactly this request token (a notification's), or
 * does nothing if it is no longer pending. */
- (void)openRequest:(NSString *)token;
- (void)refreshSection:(NSString *)section;
@end
