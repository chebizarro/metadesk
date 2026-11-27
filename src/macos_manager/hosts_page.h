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
/* The single predicate for each command (menus, buttons and handlers). */
- (BOOL)canPerform:(SEL)action;
/* Commands may bring Hosts forward (not closing, no client-role edit). */
@property(nonatomic, readonly) BOOL canNavigate;
/* Relay setup, hosted by the Relays section (its one home). */
- (NSView *)relaySettingsView;
/* The main window: sheets are presented on it, and its attached sheet gates
 * commands, whichever section is visible. */
@property(nonatomic, weak) NSWindow *presentingWindow;
/* First-run navigation, called explicitly (never through a nil target). */
@property(nonatomic, copy) void (^showIdentity)(BOOL addAccount);
@property(nonatomic, copy) dispatch_block_t showRelays;
/* Identity reloaded accounts and roles; clientKey is the client role's key
 * ("" when unassigned, nil when the accounts could not be read). Re-evaluates first-run guidance and reopens the book
 * if the client role changed under it. */
- (void)identityChanged:(NSString *)clientKey;
/* The Relays section appeared: show the persisted relay list. */
- (void)relaysAppeared;
/* Test seam: counts saved signer accounts on the worker queue. */
@property(nonatomic, copy) NSUInteger (^accountCount)(void);
- (BOOL)validateToolbarItem:(NSToolbarItem *)item;
@end
