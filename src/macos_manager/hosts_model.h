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
@property(nonatomic, readonly) BOOL cachedDetailsMayBeOutdated;
@end

/* Which condition the address book is in, for the UI to explain it and offer
 * the next step. Read-only: it does not change what canReview/canEdit allow. */
typedef NS_ENUM(NSInteger, MDHostsState) {
    MDHostsOpening,            /* not opened yet */
    MDHostsNoClientRole,       /* no account holds the client role */
    MDHostsCredentialFailed,   /* local Keychain pairing for the client signer failed */
    MDHostsSignerUnavailable,  /* client signer locked, unreachable or refused */
    MDHostsCacheUnavailable,   /* private encrypted cache could not be opened */
    MDHostsNoRelays,           /* no bootstrap relays chosen: cached only */
    MDHostsSyncing,            /* catching up */
    MDHostsRelaysOffline,      /* relays unreachable: cached only */
    MDHostsRelayRefused,       /* a relay refused the subscription or authentication */
    MDHostsAuthPending,        /* waiting for relay authentication */
    MDHostsReplayNeeded,       /* catch-up incomplete; refresh to replay */
    MDHostsDocumentError,      /* an encrypted document is invalid or unreadable */
    MDHostsSynced,
};
@interface MDHostsBook : NSObject
@property(nonatomic, readonly) NSString *owner, *status, *unavailable;
@property(nonatomic, readonly) MDHostsState state;
/* canReview is the stricter, name-based catch-up gate. */
@property(nonatomic, readonly) BOOL canReview, canReviewKey, canEdit, canExport;
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

/* Worker-only. nil input loads; a non-nil field saves explicit choices (including empty). */
NSString *MDHostsBootstrap(NSString *input, NSString **error);

NSString *MDHostCapsText(uint8_t caps);
NSString *MDHostLaunchText(MdLaunchMessage message, MdLaunchMessage phase);
NSString *MDHostLaunchDiagnostic(MdLaunchMessage message);
MdRecentOutcome MDHostLaunchOutcome(MdLaunchMessage message, MdLaunchMessage phase);
NSString *MDHostClientPath(NSString *manager, NSString *bundle, NSString *path);
