#import <AppKit/AppKit.h>

@class MDHostsWorker;
@interface MDHostsPage : NSViewController
- (instancetype)initWithWorker:(MDHostsWorker *)worker;
- (void)activate;
- (void)suspendForRoleEdit:(dispatch_block_t)ready;
- (void)resumeAfterRoleEdit;
- (void)close:(dispatch_block_t)drained;
@end
