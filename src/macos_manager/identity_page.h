#import <AppKit/AppKit.h>
#import "identity_model.h"
@class MDHostsPage;
@interface MDIdentityPage : NSViewController <NSMenuItemValidation, NSUserInterfaceValidations>
- (instancetype)initWithHosts:(MDHostsPage *)hosts;
/* Factory returns an owned credential store for each worker operation. */
- (instancetype)initWithHosts:(MDHostsPage *)hosts directory:(NSString *)directory
                      connect:(MDIdentityConnect)connect credentials:(MdCredentialStore *(^)(void))credentials;
- (void)activate;
- (void)close:(dispatch_block_t)drained;
/* Reloads saved accounts (View > Refresh). */
- (void)refresh:(id)sender;
/* Opens Add Signer Account now, or as soon as accounts have loaded. */
- (void)beginAddAccount;
/* Called on the main queue after any accounts or roles reload. */
@property(nonatomic, copy) dispatch_block_t accountsChanged;
/* Keys (hex) holding each role after the last reload: "" when unassigned,
 * nil when the saved accounts could not be read. */
@property(nonatomic, readonly) NSString *clientRoleKey, *hostRoleKey;
@end
