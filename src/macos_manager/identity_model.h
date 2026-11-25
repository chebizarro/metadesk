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
BOOL MDIdentitySupportsBackend(MdSignerType backend);
NSString *MDIdentityAdd(NSString *directory, MdSignerType backend, NSString *publicKey, NSString *label);
NSString *MDIdentityLabel(NSString *directory, NSDictionary *account, NSString *label);
/* Borrowed JSON bytes, including their NUL terminator. Caller wipes it on
 * every path. This only imports transport credentials; it does not bind a role. */
enum { MDIdentityPairingMax = 65536 };
NSString *MDIdentityImportPairing(NSString *directory, MdCredentialStore *store, NSDictionary *account,
    const char *json, size_t length, MDIdentityCancelled cancelled, void *context);
NSString *MDIdentityImportPairingFile(NSString *directory, MdCredentialStore *store, NSDictionary *account,
    NSURL *url, MDIdentityCancelled cancelled, void *context);
NSString *MDIdentityRemove(NSString *directory, MdCredentialStore *store, NSDictionary *account);
