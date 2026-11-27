#import <AppKit/AppKit.h>

@class MDHostsWorker;
@interface MDHostsPage : NSViewController <NSMenuItemValidation, NSUserInterfaceValidations>
- (instancetype)initWithWorker:(MDHostsWorker *)worker;
- (void)activate;
- (void)suspendForRoleEdit:(dispatch_block_t)ready;
- (void)resumeAfterRoleEdit;
- (void)close:(dispatch_block_t)drained;
/* Menu and toolbar commands; validated by -validateUserInterfaceItem:. */
- (void)refresh:(id)sender;
- (void)focusHostEntry:(id)sender;
- (void)focusSearch:(id)sender;
- (void)scanQR:(id)sender;
- (void)importHosts:(id)sender;
- (void)exportHosts:(id)sender;
/* Toolbar search text for the saved-host list. */
- (void)setSearchText:(NSString *)text;
- (BOOL)validateToolbarItem:(NSToolbarItem *)item;
@end
