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
/* A host role may have changed: discover the standard socket again (unless the
 * user chose a custom socket, which works with or without a host role). */
- (void)identityChanged;
/* Discovery seam (tests, renderer). Runs off the main queue; returns the
 * standard socket path and sets *hasRole. Default reads the saved host role. */
@property(nonatomic, copy) NSString *(^discover)(BOOL *hasRole);
/* Opens the review for exactly this request token (a notification's), or
 * does nothing if it is no longer pending. */
- (void)openRequest:(NSString *)token;
- (void)refreshSection:(NSString *)section;
@end
