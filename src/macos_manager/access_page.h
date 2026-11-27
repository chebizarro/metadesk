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
- (void)refreshSection:(NSString *)section;
@end
