#import <AppKit/AppKit.h>
@class MDHostsPage, MDLocalHostPages, MDIdentityPage;

@interface MDManagerApp : NSObject <NSApplicationDelegate, NSTabViewDelegate>
- (instancetype)initWithHosts:(MDHostsPage *)hosts local:(MDLocalHostPages *)local identity:(MDIdentityPage *)identity;
/* AppKit's final reply can be observed in process without exiting the test. */
- (void)replyToTermination;
@end
