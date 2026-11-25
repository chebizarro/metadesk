#import <AppKit/AppKit.h>
@class MDHostsPage;
@interface MDIdentityPage : NSViewController
- (instancetype)initWithHosts:(MDHostsPage *)hosts;
- (void)activate;
- (void)close:(dispatch_block_t)drained;
@end
