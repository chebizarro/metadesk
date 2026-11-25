#import <Foundation/Foundation.h>
#include "managed_launch.h"
#include "managed_store_controller.h"

typedef NS_ENUM(NSUInteger, MDHostInput) { MDHostInvalid, MDHostPublic, MDHostName, MDHostSecret };
MDHostInput MDClassifyHostInput(NSString *value);
BOOL MDHostRouteValid(NSString *value);
NSString *MDHostTrim(NSString *value);
NSString *MDHostNpub(NSString *key);
NSString *MDHostFingerprint(NSString *key);
NSString *MDHostBoundText(NSString *value, NSUInteger limit);

@interface MDHostReview : NSObject
@property(nonatomic, readonly) NSString *key, *npub, *fingerprint, *alias, *oldKey, *route;
@property(nonatomic, readonly) uint8_t caps;
@end

@interface MDHostsBook : NSObject
@property(nonatomic, readonly) NSString *owner, *status, *unavailable;
@property(nonatomic, readonly) BOOL canReview, canEdit, canExport;
@property(nonatomic, readonly) NSArray<NSDictionary *> *rows;
@property(nonatomic, readonly) NSArray<NSDictionary *> *quick;
@property(nonatomic, readonly) NSArray<NSDictionary *> *issues;
@property(nonatomic, readonly) NSArray<NSDictionary *> *tags;
- (NSArray<NSDictionary *> *)rowsMatching:(NSString *)query tag:(NSString *)tag;
- (NSString *)addressForKey:(NSString *)key;
- (NSString *)conflictText:(NSString *)coordinate;
- (BOOL)canReplaceReview:(MDHostReview *)review;
- (BOOL)canConfirmReview:(MDHostReview *)review fingerprint:(NSString *)fingerprint;
/* Constructed from a resolved identity (no network). Used by worker and tests. */
- (MDHostReview *)reviewIdentity:(const MdIdentity *)identity alias:(NSString *)alias route:(NSString *)route error:(NSString **)error;
@end

/* Start, read, mutate and close on a single worker. changed is a cross-thread
 * wakeup hint only; never reenter the controller in that callback. */
@interface MDHostsWorker : NSObject
- (instancetype)initWithChange:(dispatch_block_t)changed;
- (BOOL)openDirectory:(NSString *)directory error:(NSString **)error;
/* Test/fixture seam: same controller with an already verified binding. */
- (BOOL)openBinding:(MdAccountBinding *)binding directory:(NSString *)directory;
- (BOOL)startRelays:(NSString *)urls;
- (MDHostsBook *)snapshot;
- (void)close;
- (MDHostReview *)resolve:(NSString *)value route:(NSString *)route
              cancelled:(bool (*)(void *))cancelled data:(void *)data error:(NSString **)error;
- (BOOL)launchConfig:(MdManagedLaunch *)config review:(MDHostReview *)review error:(NSString **)error;
- (NSString *)saveReview:(MDHostReview *)review confirmation:(NSString *)fingerprint;
- (NSString *)editKey:(NSString *)key book:(MDHostsBook *)book label:(NSString *)label
                notes:(NSString *)notes favorite:(BOOL)favorite tagIDs:(NSSet<NSString *> *)tagIDs
               newTag:(NSString *)newTag remove:(BOOL)remove;
- (NSString *)resolveIssue:(NSString *)coordinate book:(MDHostsBook *)book reapply:(BOOL)reapply;
- (NSString *)retryIssue:(NSString *)coordinate book:(MDHostsBook *)book;
- (NSArray<NSDictionary *> *)readImport:(NSURL *)url book:(MDHostsBook *)book;
- (NSString *)applyImport:(NSArray<NSDictionary *> *)items selected:(NSSet<NSString *> *)keys book:(MDHostsBook *)book;
- (NSString *)exportBook:(MDHostsBook *)book to:(NSURL *)url;
- (void)recordKey:(NSString *)key owner:(NSString *)owner outcome:(MdRecentOutcome)outcome;
@end

NSString *MDHostCapsText(uint8_t caps);
NSString *MDHostLaunchText(MdLaunchMessage message, MdLaunchMessage phase);
NSString *MDHostLaunchDiagnostic(MdLaunchMessage message);
MdRecentOutcome MDHostLaunchOutcome(MdLaunchMessage message, MdLaunchMessage phase);
NSString *MDHostClientPath(NSString *manager, NSString *bundle, NSString *path);
