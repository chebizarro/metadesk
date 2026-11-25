#import <AppKit/AppKit.h>

@interface MDLocalHostPages : NSObject
@property(nonatomic, readonly) NSView *computerView, *accessView;
- (void)close:(dispatch_block_t)drained;
@end
