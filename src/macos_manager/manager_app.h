#import <AppKit/AppKit.h>
@class MDHostsPage, MDLocalHostPages, MDIdentityPage;

@interface MDManagerApp : NSObject <NSApplicationDelegate, NSTabViewDelegate, NSMenuItemValidation, NSUserInterfaceValidations>
- (instancetype)initWithHosts:(MDHostsPage *)hosts local:(MDLocalHostPages *)local identity:(MDIdentityPage *)identity;
/* The standard menu bar. Items have nil targets and travel the responder chain. */
+ (NSMenu *)makeMainMenu;
+ (void)installMainMenu:(NSApplication *)app;
/* Section identifiers and titles, in sidebar and View menu order. */
+ (NSArray<NSString *> *)sectionTitles;
- (void)showSection:(id)sender;
/* AppKit's final reply can be observed in process without exiting the test. */
- (void)replyToTermination;
@end
