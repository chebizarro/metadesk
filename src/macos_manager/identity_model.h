#import <Foundation/Foundation.h>
#include "accounts.h"

typedef int (*MDIdentityConnect)(MdSignerType, const char *, MdCredentialStore *, MdSigner **);
typedef BOOL (*MDIdentityNode)(char out[65], void *);
typedef BOOL (*MDIdentityCancelled)(void *);

NSArray<NSDictionary *> *MDIdentityAccounts(NSString *directory, NSString **error);
NSString *MDIdentityError(int rc, MdSignerType backend);
BOOL MDIdentityLocalNode(char out[65], void *unused);
/* expected is the previous role's stable account ID, or the empty string for
 * unassigned. A nil account unbinds. The store and seams are borrowed. */
NSString *MDIdentityBind(NSString *directory, MdCredentialStore *store, MdAccountRole role,
    NSDictionary *account, NSString *expected, BOOL fips, MDIdentityConnect connect,
    MDIdentityNode node, MDIdentityCancelled cancelled, void *context);
NSString *MDIdentityRemove(NSString *directory, MdCredentialStore *store, NSDictionary *account);
