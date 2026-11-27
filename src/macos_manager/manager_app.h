#import <AppKit/AppKit.h>
@class MDHostsPage, MDLocalHostPages, MDIdentityPage;

@interface MDManagerApp : NSObject <NSApplicationDelegate, NSMenuItemValidation, NSToolbarItemValidation, NSUserInterfaceValidations>
- (instancetype)initWithHosts:(MDHostsPage *)hosts local:(MDLocalHostPages *)local identity:(MDIdentityPage *)identity;
/* The standard menu bar. Items have nil targets and travel the responder chain. */
+ (NSMenu *)makeMainMenu;
+ (void)installMainMenu:(NSApplication *)app;
/* Section identifiers and titles, in sidebar and View menu order. */
+ (NSArray<NSString *> *)sectionTitles;
- (void)showSection:(id)sender;
/* Builds the pages and main window without showing it (used by launch and the
 * offscreen snapshot renderer). */
- (NSWindow *)makeMainWindow;
- (NSArray<NSString *> *)sectionIdentifiers;
/* Test hook: the section makeMainWindow shows first. nil (always, in the
 * app) means Hosts. */
@property(nonatomic, copy) NSString *initialSection;
- (NSValue *)defaultContentSize;
/* AppKit's final reply can be observed in process without exiting the test. */
- (void)replyToTermination;
@end
