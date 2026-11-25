#import <Foundation/Foundation.h>
#include "management_ipc.h"

NSDictionary *MDParseHostFrame(NSData *data);
NSArray<NSDictionary *> *MDLiveRequests(NSDictionary *snapshot, NSTimeInterval now);
NSString *MDRequestToken(NSDictionary *snapshot, NSDictionary *request);
NSSet<NSString *> *MDRequestNotices(NSDictionary *snapshot, NSTimeInterval now);
NSDictionary *MDRequestForToken(NSDictionary *snapshot, NSString *token, NSTimeInterval now);
uint32_t MDApprovalAllowed(NSDictionary *snapshot, NSDictionary *request);
uint32_t MDApprovalDefault(NSDictionary *snapshot, NSDictionary *request);
NSString *MDAccessCaps(uint32_t caps);
BOOL MDPolicyCaps(NSString *text, uint32_t *out);
NSDictionary *MDApprovalCommand(NSDictionary *snapshot, NSDictionary *request, NSString *decision,
                                uint32_t caps, BOOL remember, NSTimeInterval now);
NSDictionary *MDPolicyCommand(NSDictionary *snapshot, NSDictionary *policy, NSDictionary *client,
                              uint32_t caps, BOOL remove, NSString *sessionID);
NSDictionary *MDKickCommand(NSDictionary *snapshot, NSDictionary *session);

typedef void (^MDHostUpdate)(NSString *status, NSDictionary *snapshot, NSDictionary *policy,
                             NSDictionary *permissions, BOOL busy);
@interface MDHostLink : NSObject
- (instancetype)initWithUpdate:(MDHostUpdate)update;
- (void)connect:(NSString *)path;
- (void)command:(NSDictionary *)command;
- (void)command:(NSDictionary *)command policyVersion:(NSNumber *)expected;
- (void)close:(dispatch_block_t)drained;
@end
