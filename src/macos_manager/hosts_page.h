#import <AppKit/AppKit.h>

@interface MDHostsPage : NSViewController
- (void)activate;
- (void)suspendForRoleEdit:(dispatch_block_t)ready;
- (void)resumeAfterRoleEdit;
- (void)close:(dispatch_block_t)drained;
@end
