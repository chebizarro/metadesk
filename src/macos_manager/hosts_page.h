#import <AppKit/AppKit.h>

@interface MDHostsPage : NSViewController
- (void)activate;
- (void)close:(dispatch_block_t)drained;
@end
