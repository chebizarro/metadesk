#import "hosts_model.h"

/* Read and spawn are kept off the AppKit and store queues. All update and
 * close callbacks are delivered on the main queue. */
@interface MDHostsLaunch : NSObject
- (instancetype)initWithUpdate:(void (^)(MdLaunchMessage))update;
- (void)start:(const MdManagedLaunch *)config manager:(NSString *)manager bundle:(NSString *)bundle path:(NSString *)path;
- (void)control;
- (void)close:(dispatch_block_t)completion;
@end
