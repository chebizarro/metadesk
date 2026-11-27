/* Test-only offscreen renderer for design review (md-b9zo).
 *
 * Builds the real page controllers with fixture workers, lays the main window
 * out at its default and minimum sizes in light and dark appearance, opens each
 * sheet, and renders everything to PNG with cacheDisplayInRect:. No window is
 * ordered on screen, nothing is activated, no pasteboard, Keychain, signer,
 * relay or host IPC is used. Usage: macos_manager_snapshots MESON_BUILD_DIR.
 * It renders into a fresh BUILD/snapshots/run-XXXXXX that it creates itself
 * and confines every home, config, state and output path to (see Launch). */
#import "manager_app.h"
#import "hosts_page.h"
#import "hosts_model.h"
#import "access_page.h"
#import "identity_page.h"
#import "ui.h"
#include <stdatomic.h>
#include <objc/message.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>

#define KEY1 "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define KEY2 "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
#define KEY3 "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9"
#define KEY4 "e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd13"
#define GEN "abababababababababababababababab"
#define REQUEST "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"

static NSString *Out;
/* Descriptors for the fixture output and config directories, opened by
 * Prepare; files are created relative to them, never by pathname. */
static int OutFD = -1, ConfigFD = -1;
/* Creates a new file named name (a single component) in OutFD exclusively:
 * never follows a link, never truncates. */
static BOOL WriteNew(NSString *name, NSData *data) {
    const char *leaf = name.fileSystemRepresentation;
    if (!leaf || !*leaf || strchr(leaf, '/') || OutFD < 0) return NO;
    int fd = openat(OutFD, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return NO;
    const uint8_t *bytes = data.bytes; size_t done = 0;
    while (done < data.length) {
        ssize_t n = write(fd, bytes + done, data.length - done);
        if (n <= 0) { close(fd); return NO; }
        done += (size_t)n;
    }
    return close(fd) == 0;
}
static int Failures;
static NSString *Slug(NSString *section);
static NSMutableArray<NSString *> *Written;

static BOOL Await(BOOL (^ready)(void)) {
    CFAbsoluteTime limit = CFAbsoluteTimeGetCurrent() + 4;
    while (!ready()) {
        CFTimeInterval left = limit - CFAbsoluteTimeGetCurrent();
        if (left <= 0) return NO;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, MIN(left, 0.05), true);
    }
    return YES;
}
static void Spin(void) { for (int i = 0; i < 4; ++i) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false); }
static NSView *Find(NSView *root, BOOL (^matches)(NSView *)) {
    if (root && matches(root)) return root;
    for (NSView *view in root.subviews) { NSView *hit = Find(view, matches); if (hit) return hit; }
    return nil;
}
static NSBitmapImageRep *Cache(NSView *view) {
    NSBitmapImageRep *rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    return rep;
}
static void Effects(NSView *root, NSMutableArray<NSView *> *out) {
    for (NSView *v in root.subviews) {
        if (v.isHidden) continue;
        if ([NSStringFromClass(v.class) hasSuffix:@"EffectView"] && v.frame.size.width < root.window.contentView.bounds.size.width / 2) [out addObject:v];
        else Effects(v, out);
    }
}
/* cacheDisplayInRect: does not draw what an NSVisualEffectView or (macOS 26+)
 * NSGlassEffectView hosts: the sidebar material is composited by the window
 * server. Approximate the
 * material with a flat semantic color and draw the hosted views on top. */
static void Composite(NSView *view, NSBitmapImageRep *rep) {
    NSMutableArray<NSView *> *effects = [NSMutableArray array];
    Effects(view, effects);
    if (!effects.count) return;
    NSGraphicsContext *context = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = context;
    for (NSView *effect in effects) {
        NSTableView *table = (NSTableView *)Find(effect, ^BOOL(NSView *v) { return [v isKindOfClass:NSTableView.class]; });
        if (!table) continue;
        NSScrollView *scroll = table.enclosingScrollView;
        NSRect r = [view convertRect:[scroll convertRect:scroll.bounds toView:nil] fromView:nil];
        if (view.isFlipped) r.origin.y = view.bounds.size.height - NSMaxY(r);
        if (!view.isFlipped) r = NSIntersectionRect(r, NSMakeRect(0, 0, view.bounds.size.width, NSMaxY(view.window.contentLayoutRect)));
        BOOL dark = [effect.effectiveAppearance bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]] == NSAppearanceNameDarkAqua;
        [(dark ? [NSColor colorWithWhite:0.17 alpha:1] : [NSColor colorWithWhite:0.93 alpha:1]) setFill];
        NSRectFill(r);
        for (NSInteger row = 0; row < table.numberOfRows; ++row) {
            NSTableRowView *rowView = [table rowViewAtRow:row makeIfNecessary:YES];
            if (!rowView || NSIsEmptyRect(rowView.bounds)) continue;
            [rowView layoutSubtreeIfNeeded];
            NSRect c = [view convertRect:[rowView convertRect:rowView.bounds toView:nil] fromView:nil];
            if (view.isFlipped) c.origin.y = view.bounds.size.height - NSMaxY(c);
            if (rowView.isSelected) {
                [(dark ? [NSColor colorWithWhite:1 alpha:0.12] : [NSColor colorWithWhite:0 alpha:0.08]) setFill];
                [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(c, 8, 1) xRadius:6 yRadius:6] fill];
            }
            for (NSView *cell in rowView.subviews) {
                if (![cell isKindOfClass:NSTableCellView.class]) continue;
                NSBitmapImageRep *part = Cache(cell);
                NSRect cr = [view convertRect:[cell convertRect:cell.bounds toView:nil] fromView:nil];
                if (view.isFlipped) cr.origin.y = view.bounds.size.height - NSMaxY(cr);
                [part drawInRect:cr fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
            }
        }
    }
    [context flushGraphics];
    [NSGraphicsContext restoreGraphicsState];
}
static void Render(NSView *view, NSString *name) {
    [view.window layoutIfNeeded];
    [view layoutSubtreeIfNeeded];
    __block NSBitmapImageRep *rep = nil;
    NSAppearance *appearance = view.effectiveAppearance;
    [appearance performAsCurrentDrawingAppearance:^{
        rep = Cache(view);
        Composite(view, rep);
    }];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSString *file = [name stringByAppendingPathExtension:@"png"];
    if (!png || !WriteNew(file, png)) { fprintf(stderr, "FAIL: could not write %s\n", file.UTF8String); ++Failures; return; }
    [Written addObject:file];
}
/* The frame view includes the title bar and toolbar. */
static NSView *Frame(NSWindow *window) { return window.contentView.superview ?: window.contentView; }

@interface SnapReview : MDHostReview
@property BOOL mismatch;
@end
@implementation SnapReview
- (NSString *)key { return @KEY2; }
- (NSString *)npub { return MDHostNpub(self.key); }
- (NSString *)fingerprint { return MDHostFingerprint(self.key); }
- (NSString *)oldKey { return self.mismatch ? @KEY3 : nil; }
- (NSString *)alias { return @"render@example.invalid"; }
- (uint8_t)caps { return MD_CAP_VIDEO | MD_CAP_INPUT; }
@end

@interface SnapBook : MDHostsBook
@property BOOL empty;
@property MDHostsState fixtureState;
@end
@implementation SnapBook
static NSArray *SnapRows(void) {
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    return @[
        @{@"key":@KEY1, @"title":@"Studio Mac", @"label":@"Studio Mac", @"alias":@"studio@example.invalid",
          @"notes":@"Second floor, colour-calibrated display.", @"tags":@[@"Office"], @"tagIDs":@[@"t1"], @"favorite":@YES, @"lastConnected":@(now - 3600)},
        @{@"key":@KEY2, @"title":@"Render Box", @"label":@"Render Box", @"alias":@"render@example.invalid", @"notes":@"",
          @"tags":@[@"Office", @"Linux"], @"tagIDs":@[@"t1", @"t2"], @"favorite":@NO, @"lastConnected":@(now - 86400 * 3)},
        @{@"key":@KEY3, @"title":MDHostFingerprint(@KEY3), @"label":@"", @"alias":@"", @"notes":@"",
          @"tags":@[], @"tagIDs":@[], @"favorite":@NO, @"lastConnected":@0},
        @{@"key":@KEY4, @"title":@"Workshop laptop with an unusually long descriptive label that has to truncate somewhere",
          @"label":@"Workshop laptop with an unusually long descriptive label that has to truncate somewhere", @"alias":@"",
          @"notes":@"Bench by the window. Ask before connecting during the day; the owner presents from it.",
          @"tags":@[@"Linux"], @"tagIDs":@[@"t2"], @"favorite":@YES, @"lastConnected":@0},
    ];
}
- (BOOL)blocked { return self.fixtureState >= MDHostsNoClientRole && self.fixtureState <= MDHostsCacheUnavailable; }
- (BOOL)synced { return self.fixtureState == MDHostsSynced; }
- (MDHostsState)state { return self.fixtureState; }
- (NSArray *)rows { return self.empty || self.blocked ? @[] : SnapRows(); }
- (NSArray *)quick { return self.empty || self.blocked ? @[] : @[SnapRows()[0], SnapRows()[3], SnapRows()[1]]; }
- (NSArray *)tags { return self.empty || self.blocked ? @[] : @[@{@"id":@"t2", @"label":@"Linux"}, @{@"id":@"t1", @"label":@"Office"}]; }
- (NSArray *)issues {
    return !self.synced ? @[] : @[@{@"coordinate":@"30078:" KEY1 ":metadesk-hosts", @"message":@"Your edit conflicts with a newer remote version.",
        @"conflict":@YES, @"retry":@NO, @"ready":@YES}];
}
- (NSString *)unavailable {
    switch (self.fixtureState) {
        case MDHostsNoClientRole: return @"Assign a client role with the account CLI, then refresh.";
        case MDHostsSignerUnavailable: return @"Client signer unavailable: signer locked.";
        case MDHostsCredentialFailed: return @"Local NIP-46 credential access failed: not found. Check Keychain and the saved pairing; no external signer was reached.";
        case MDHostsCacheUnavailable: return @"The encrypted cache is unavailable. Check the private state directory and other manager instances.";
        default: return nil;
    }
}
- (NSString *)status {
    switch (self.fixtureState) {
        case MDHostsNoRelays: return @"Offline — showing cached hosts. Add explicit bootstrap relays to sync.";
        case MDHostsSyncing: return @"Syncing address book…";
        case MDHostsAuthPending: return @"Waiting for relay authentication…";
        case MDHostsRelaysOffline: return @"Offline — showing cached hosts.";
        case MDHostsRelayRefused: return @"An address book relay refused the subscription.";
        case MDHostsReplayNeeded: return @"Address book catch-up was incomplete. Refresh to replay it.";
        case MDHostsDocumentError: return @"An encrypted document is invalid and will not be overwritten.";
        case MDHostsSynced: return @"Address book synced.";
        default: return self.unavailable ?: @"Opening the address book…";
    }
}
- (NSString *)owner { return self.blocked ? nil : @KEY1; }
- (BOOL)canReview { return self.synced; }
- (BOOL)canEdit { return self.synced; }
- (BOOL)canExport { return !self.blocked && !self.empty; }
- (BOOL)canReplaceReview:(MDHostReview *)review { return review.oldKey != nil; }
- (BOOL)canConfirmReview:(MDHostReview *)review fingerprint:(NSString *)fingerprint {
    return review.oldKey && [review.fingerprint isEqual:fingerprint];
}
- (NSArray *)rowsMatching:(NSString *)query tag:(NSString *)tag {
    NSMutableArray *out = [NSMutableArray array];
    for (NSDictionary *row in self.rows) {
        if (tag.length && ![row[@"tagIDs"] containsObject:tag]) continue;
        if (query.length && ![row[@"title"] localizedCaseInsensitiveContainsString:query]) continue;
        [out addObject:row];
    }
    return out;
}
- (NSString *)addressForKey:(NSString *)key { return key; }
- (NSString *)conflictText:(NSString *)coordinate {
    (void)coordinate;
    return @"Remote version (revision 7)\n  Studio Mac — notes: Second floor.\n  tags: Office\n\n"
           @"Your draft (based on revision 6)\n  Studio Mac — notes: Second floor, colour-calibrated display.\n  tags: Office, Favorites\n";
}
@end

@interface SnapHosts : MDHostsWorker
@property BOOL empty;
@property MDHostsState state;
@end
@implementation SnapHosts
- (BOOL)openDirectory:(NSString *)directory error:(NSString **)error { (void)directory; (void)error; return YES; }
- (MDHostsBook *)snapshot {
    if (self.state == MDHostsOpening) return nil;
    SnapBook *b = [SnapBook new]; b.empty = self.empty; b.fixtureState = self.state; return b;
}
- (BOOL)startRelays:(NSString *)urls { (void)urls; return YES; }
- (void)close {}
@end

@interface SnapLink : MDHostLink
@property(copy) MDHostUpdate push;
@end
@implementation SnapLink
- (void)connect:(NSString *)path { (void)path; }
- (void)command:(NSDictionary *)cmd policyVersion:(NSNumber *)version { (void)cmd; (void)version; }
- (void)close:(dispatch_block_t)drained { drained(); }
@end

static NSString *SignerKey;
static int SnapKey(MdSigner *s, char **key) { (void)s; *key = strdup(SignerKey.UTF8String); return *key ? 0 : MD_SIGNER_ERR_INTERNAL; }
static int SnapConnect(MdSignerType backend, const char *key, MdCredentialStore *store, MdSigner **out) {
    (void)key; (void)store;
    static const MdSignerOps ops = {.get_pubkey = SnapKey};
    *out = md_signer_create_custom(backend, &ops, NULL); return *out ? 0 : MD_SIGNER_ERR_INTERNAL;
}
static int NoPair(void *u, const char *k, char **p) { (void)u; (void)k; *p = NULL; return MD_CREDENTIAL_NOT_FOUND; }
static int PutPair(void *u, const char *k, const char *p) { (void)u; (void)k; (void)p; return MD_CREDENTIAL_INVALID; }
static int DropPair(void *u, const char *k) { (void)u; (void)k; return 0; }
static MdCredentialStore *SnapStore(void) {
    static const MdCredentialStoreOps ops = {.get = NoPair, .put = PutPair, .remove = DropPair};
    return md_credential_store_custom(&ops, NULL);
}
static BOOL NeverCancelled(void *c) { (void)c; return NO; }
static void SeedAccounts(NSString *directory) {
    MDIdentityAdd(directory, MD_SIGNER_NIP46, @KEY1, @"Phone signer");
    MDIdentityAdd(directory, MD_SIGNER_NIP5F, @KEY2, @"This Mac’s signer");
    MDIdentityAdd(directory, MD_SIGNER_NIP5F, @KEY4, @"");
    NSArray *accounts = MDIdentityAccounts(directory, NULL);
    for (NSDictionary *a in accounts) {
        MdAccountRole role;
        if ([a[@"key"] isEqual:@KEY2]) role = MD_ACCOUNT_CLIENT;
        else if ([a[@"key"] isEqual:@KEY4]) role = MD_ACCOUNT_HOST;
        else continue;
        SignerKey = a[@"key"];
        MdCredentialStore *store = SnapStore();
        NSString *error = MDIdentityBind(directory, store, role, a, @"", NO, SnapConnect, NULL, NeverCancelled, NULL);
        md_credential_store_destroy(store);
        if (error.length) { fprintf(stderr, "FAIL: fixture role: %s\n", error.UTF8String); ++Failures; }
    }
}

static NSDictionary *AccessSnapshot(NSTimeInterval now) {
    return @{@"v":@1, @"ok":@YES, @"generation":@GEN, @"revision":@3,
        @"session":@{@"session_id":@"s1", @"peer":@KEY2, @"granted":@(MD_CAP_VIDEO), @"revoked":@NO},
        @"requests":@[@{@"request_id":@REQUEST, @"peer":@KEY3, @"requested":@(MD_CAP_VIDEO | MD_CAP_INPUT), @"granted":@0,
            @"state":@"pending", @"expires_at":@(now + 45)}],
        @"supported":@(MD_CAP_VIDEO | MD_CAP_AGENT | MD_CAP_INPUT),
        @"host":@{@"managed":@YES, @"pid":@4242, @"state":@"running"}};
}
static NSDictionary *AccessPolicy(void) {
    return @{@"policy_available":@YES, @"policy_version":@4, @"generation":@GEN, @"clients":@[
        @{@"peer":@KEY2, @"caps":@"video,input", @"review_required":@NO},
        @{@"peer":@KEY4, @"caps":@"video", @"review_required":@NO}]};
}

typedef struct { MDManagerApp *app; MDHostsPage *hosts; MDLocalHostPages *local; MDIdentityPage *identity;
                 SnapHosts *worker; SnapLink *link; NSWindow *window; } Scene;

static void ShowSection(Scene *s, NSString *identifier) {
    [s->app performSelector:@selector(showSection:) withObject:identifier];
    if ([identifier isEqual:@"Hosts"]) [s->hosts activate];
    if ([identifier isEqual:@"Identity"]) [s->identity activate];
    Spin();
}
static Scene MakeScene(NSAppearanceName appearance, MDHostsState state, BOOL empty, NSInteger accounts) {
    Scene s = {0};
    s.worker = [[SnapHosts alloc] initWithChange:nil]; s.worker.state = state; s.worker.empty = empty;
    s.hosts = [[MDHostsPage alloc] initWithWorker:s.worker];
    if (accounts >= 0) s.hosts.accountCount = ^NSUInteger { return (NSUInteger)accounts; };
    SnapLink *link = [[SnapLink alloc] initWithUpdate:nil]; s.link = link;
    s.local = [[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) { link.push = update; return link; }];
    /* Never look up the real host role or runtime directory: a fixture path. */
    NSString *socket = [@(getenv("XDG_RUNTIME_DIR")) stringByAppendingPathComponent:@"fixture-host.sock"];
    s.local.discover = ^NSString *(NSString **hostKey) { *hostKey = @"fixture-host-role"; return socket; };
    s.identity = [[MDIdentityPage alloc] initWithHosts:s.hosts directory:nil connect:SnapConnect credentials:^{ return SnapStore(); }];
    s.app = [[MDManagerApp alloc] initWithHosts:s.hosts local:s.local identity:s.identity];
    s.window = [s.app makeMainWindow];
    s.window.releasedWhenClosed = NO; s.window.restorable = NO; s.window.alphaValue = 0;
    s.window.appearance = [NSAppearance appearanceNamed:appearance];
    return s;
}
/* Tear a scene down through the controllers' normal close paths. */
static void Drain(Scene *s) {
    __block int pending = 3;
    dispatch_block_t done = ^{ --pending; };
    while (s->window.attachedSheet) { [s->window endSheet:s->window.attachedSheet returnCode:NSModalResponseCancel]; Spin(); }
    [s->hosts close:done]; [s->local close:done]; [s->identity close:done];
    if (!Await(^BOOL{ return pending == 0; })) { fputs("FAIL: scene did not drain\n", stderr); ++Failures; }
    [s->window close];
    Spin();
}
static void Resize(NSWindow *window, NSSize content) {
    [window setFrame:[window frameRectForContentRect:(NSRect){NSZeroPoint, content}] display:NO];
    [window layoutIfNeeded];
}
static void FeedAccess(Scene *s) {
    s->link.push(@"Connected to the local host.", AccessSnapshot(NSDate.date.timeIntervalSince1970), AccessPolicy(),
                 @{@"screen_recording":@YES, @"accessibility":@NO}, NO);
    Spin();
}

static void CaptureSheet(Scene *s, NSString *name, void (^open)(void)) {
    NSWindow *window = s->window;
    open();
    if (!Await(^BOOL{ return window.attachedSheet != nil; })) { fprintf(stderr, "FAIL: %s did not open a sheet\n", name.UTF8String); ++Failures; return; }
    NSWindow *sheet = window.attachedSheet;
    Spin(); [sheet layoutIfNeeded];
    Render(Frame(sheet), name);
    [window endSheet:sheet returnCode:NSModalResponseCancel];
    Await(^BOOL{ return window.attachedSheet == nil; });
    Spin();
}
static void Try(Scene *s, NSString *name, id target, SEL action) {
    if (![target respondsToSelector:action]) { fprintf(stderr, "FAIL: %s has no %s\n", name.UTF8String, sel_getName(action)); ++Failures; return; }
    CaptureSheet(s, name, ^{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        [target performSelector:action withObject:nil];
#pragma clang diagnostic pop
    });
}
static NSTableView *Table(NSView *root, NSString *label) {
    return (NSTableView *)Find(root, ^BOOL(NSView *v) { return [v isKindOfClass:NSTableView.class] && [v.accessibilityLabel isEqual:label]; });
}

static void Sheets(Scene *s, NSString *suffix) {
    MDHostsPage *hosts = s->hosts;
    ShowSection(s, @"Hosts");
    Await(^BOOL{ return Table(hosts.view, @"Saved hosts").numberOfRows > 0; });
    [Table(hosts.view, @"Saved hosts") selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    Spin();
    SnapReview *first = [SnapReview new], *changed = [SnapReview new]; changed.mismatch = YES;
    NSString *(^n)(NSString *) = ^(NSString *base) { return [base stringByAppendingString:suffix]; };
    CaptureSheet(s, n(@"sheet-hosts-review-"), ^{ [hosts performSelector:@selector(presentReview:) withObject:first]; });
    CaptureSheet(s, n(@"sheet-hosts-key-changed-"), ^{ [hosts performSelector:@selector(presentReview:) withObject:changed]; });
    Try(s, n(@"sheet-hosts-edit-"), hosts, @selector(edit:));
    NSArray *imports = @[
        @{@"key":@KEY3, @"npub":MDHostNpub(@KEY3), @"fingerprint":MDHostFingerprint(@KEY3), @"label":@"Lab iMac", @"alias":@"lab@example.invalid", @"caps":@"Video (view only)"},
        @{@"key":@KEY4, @"npub":MDHostNpub(@KEY4), @"fingerprint":MDHostFingerprint(@KEY4), @"label":@"Kiosk", @"alias":@"", @"caps":@"Video, input"}];
    CaptureSheet(s, n(@"sheet-hosts-import-"), ^{ [hosts performSelector:@selector(reviewImport:book:) withObject:imports withObject:[s->worker snapshot]]; });
    Try(s, n(@"sheet-hosts-export-"), hosts, @selector(exportHosts:));
    Try(s, n(@"sheet-hosts-conflict-"), hosts, @selector(handleIssue:));
    /* Unreadable relay settings: the fixture file is created here, inside the
     * fixture config directory, exclusively, and removed only by this code. */
    if (mkdirat(ConfigFD, "metadesk", 0700) && errno != EEXIST) { fputs("FAIL: fixture config directory\n", stderr); ++Failures; }
    int dir = openat(ConfigFD, "metadesk", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    int fd = dir >= 0 && !fstat(dir, &st) && st.st_uid == geteuid() ?
        openat(dir, "manager-relays.ini", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    if (fd < 0) { fputs("FAIL: fixture relay settings already exist\n", stderr); ++Failures; }
    else {
        static const char corrupt[] = "not a relay preferences file";
        BOOL wrote = write(fd, corrupt, sizeof corrupt - 1) == (ssize_t)(sizeof corrupt - 1);
        close(fd);
        if (wrote) Try(s, n(@"sheet-hosts-unreadable-relays-"), hosts, @selector(saveRelays:));
        unlinkat(dir, "manager-relays.ini", 0);
    }
    if (dir >= 0) close(dir);
    Spin();

    ShowSection(s, @"Access");
    FeedAccess(s);
    Try(s, n(@"sheet-access-review-"), s->local, @selector(review:));
    Try(s, n(@"sheet-access-edit-"), s->local, @selector(edit:));
    Try(s, n(@"sheet-access-revoke-"), s->local, @selector(revoke:));
    Try(s, n(@"sheet-access-kick-revoke-"), s->local, @selector(kickRevoke:));
    ShowSection(s, @"This Computer");
    Try(s, n(@"sheet-computer-disconnect-"), s->local, @selector(kick:));
    Try(s, n(@"sheet-computer-qr-"), s->local, @selector(showQR:));

    ShowSection(s, @"Identity");
    Await(^BOOL{ return Table(s->identity.view, @"Signer accounts").numberOfRows > 0; });
    Try(s, n(@"sheet-identity-add-"), s->identity, @selector(add:));
    Try(s, n(@"sheet-identity-label-"), s->identity, @selector(editLabel:));
    Try(s, n(@"sheet-identity-pairing-"), s->identity, @selector(importPairing:));
    Try(s, n(@"sheet-identity-remove-"), s->identity, @selector(remove:));
}

static void Pages(NSAppearanceName appearance, NSString *mode) {
    Scene s = MakeScene(appearance, MDHostsSynced, NO, -1);
    NSSize sizes[2] = {[MDManagerApp.new defaultContentSize].sizeValue, s.window.contentMinSize};
    NSString *names[2] = {@"default", @"minimum"};
    Resize(s.window, sizes[0]);
    for (NSString *section in @[@"This Computer", @"Access"]) {
        ShowSection(&s, section);
        Render(Frame(s.window), [NSString stringWithFormat:@"page-%@-disconnected-default-%@", Slug(section), mode]);
    }
    for (int z = 0; z < 2; ++z) {
        Resize(s.window, sizes[z]);
        for (NSString *section in [s.app sectionIdentifiers]) {
            ShowSection(&s, section);
            if ([section isEqual:@"Hosts"]) Await(^BOOL{ return Table(s.hosts.view, @"Saved hosts").numberOfRows > 0; });
            if ([section isEqual:@"Identity"]) Await(^BOOL{ return Table(s.identity.view, @"Signer accounts").numberOfRows > 0; });
            if ([section isEqual:@"This Computer"] || [section isEqual:@"Access"]) FeedAccess(&s);
            if ([section isEqual:@"Hosts"]) {
                NSTableView *t = Table(s.hosts.view, @"Saved hosts");
                if (t.numberOfRows) [t selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
                Spin();
            }
            Render(Frame(s.window), [NSString stringWithFormat:@"page-%@-%@-%@", Slug(section), names[z], mode]);
        }
    }
    /* A connection in progress on Hosts. */
    Resize(s.window, sizes[0]);
    ShowSection(&s, @"Hosts");
    [s.hosts setValue:@YES forKey:@"launched"];
    [s.hosts performSelector:@selector(render)];
    ((void (*)(id, SEL, NSString *, NSInteger))objc_msgSend)(s.hosts, @selector(showConnection:kind:), MDHostLaunchText(MD_LAUNCH_APPROVAL, MD_LAUNCH_RELAY), MDStatusWorking);
    Render(Frame(s.window), [@"page-hosts-connecting-default-" stringByAppendingString:mode]);
    [s.hosts setValue:@NO forKey:@"launched"];
    ((void (*)(id, SEL, NSString *, NSInteger))objc_msgSend)(s.hosts, @selector(showConnection:kind:), nil, MDStatusNone);
    Sheets(&s, mode);
    Drain(&s);

    /* This Computer when the host can't be reached, and with no host role. */
    Scene c = MakeScene(appearance, MDHostsSynced, NO, -1);
    Resize(c.window, sizes[0]);
    ShowSection(&c, @"This Computer");
    [c.local activate];
    Await(^BOOL{ return [[c.local valueForKey:@"located"] boolValue]; });
    c.link.push(@"Host unreachable or socket not trusted. Agent process state is unknown.", nil, nil, nil, NO);
    Spin();
    Render(Frame(c.window), [@"page-this-computer-unreachable-default-" stringByAppendingString:mode]);
    c.local.discover = ^NSString *(NSString **hostKey) { *hostKey = nil; return nil; };
    [c.local hostRoleChanged:@""];
    Await(^BOOL{ return [[c.local valueForKey:@"located"] boolValue] && [[c.local valueForKey:@"noHostRole"] boolValue]; });
    c.link.push(nil, nil, nil, nil, NO);
    Spin();
    Render(Frame(c.window), [@"page-this-computer-no-host-role-default-" stringByAppendingString:mode]);
    Drain(&c);

    /* Every unconfigured or degraded Hosts state a new install can meet. */
    struct { MDHostsState state; NSInteger accounts; const char *name; BOOL empty; } states[] = {
        {MDHostsOpening, 1, "opening", YES}, {MDHostsNoClientRole, 0, "no-signer-account", YES},
        {MDHostsNoClientRole, 2, "no-client-role", YES}, {MDHostsSignerUnavailable, 2, "signer-unavailable", YES},
        {MDHostsCredentialFailed, 2, "pairing-unavailable", YES}, {MDHostsCacheUnavailable, 2, "cache-unavailable", YES},
        {MDHostsNoRelays, 2, "no-relays", YES}, {MDHostsSyncing, 2, "syncing", NO}, {MDHostsAuthPending, 2, "relay-sign-in", NO},
        {MDHostsRelaysOffline, 2, "relays-offline", NO}, {MDHostsRelayRefused, 2, "relay-refused", NO},
        {MDHostsReplayNeeded, 2, "catch-up-incomplete", NO}, {MDHostsDocumentError, 2, "unreadable", NO},
    };
    for (size_t i = 0; i < sizeof states / sizeof *states; ++i) {
        Scene e = MakeScene(appearance, states[i].state, states[i].empty, states[i].accounts);
        Resize(e.window, sizes[0]);
        ShowSection(&e, @"Hosts");
        if (states[i].state != MDHostsOpening) Await(^BOOL{ return [e.hosts valueForKey:@"book"] != nil; });
        Spin();
        Render(Frame(e.window), [NSString stringWithFormat:@"page-hosts-state-%s-default-%@", states[i].name, mode]);
        if (i == 6 || i == 9) {
            Resize(e.window, sizes[1]); Spin();
            Render(Frame(e.window), [NSString stringWithFormat:@"page-hosts-state-%s-minimum-%@", states[i].name, mode]);
        }
        Drain(&e);
    }
}
static NSString *Slug(NSString *section) { return [[section lowercaseString] stringByReplacingOccurrencesOfString:@" " withString:@"-"]; }

static void DumpMenu(NSMenu *menu, NSMutableString *out, int depth) {
    for (NSMenuItem *item in menu.itemArray) {
        NSMutableString *line = [NSMutableString string];
        for (int i = 0; i < depth; ++i) [line appendString:@"    "];
        if (item.isSeparatorItem) { [line appendString:@"----"]; [out appendFormat:@"%@\n", line]; continue; }
        [line appendString:item.title.length ? item.title : @"(application menu)"];
        if (item.keyEquivalent.length) {
            NSMutableString *key = [NSMutableString string];
            NSEventModifierFlags m = item.keyEquivalentModifierMask;
            if (m & NSEventModifierFlagControl) [key appendString:@"⌃"];
            if (m & NSEventModifierFlagOption) [key appendString:@"⌥"];
            if (m & NSEventModifierFlagShift) [key appendString:@"⇧"];
            if (m & NSEventModifierFlagCommand) [key appendString:@"⌘"];
            [key appendString:item.keyEquivalent.uppercaseString];
            [line appendFormat:@"\t%@", key];
        }
        if (item.action) [line appendFormat:@"\t[%@]", NSStringFromSelector(item.action)];
        [out appendFormat:@"%@\n", line];
        if (item.submenu) DumpMenu(item.submenu, out, depth + 1);
    }
}

/* ---- Fixture-root safety (md-b9zo B3, R2, round 4) ----
 * One entry point, one argument: a canonical meson build directory owned by
 * this user. Before any Foundation or AppKit call, the renderer opens that
 * directory one component at a time from "/" with O_NOFOLLOW | O_DIRECTORY
 * (so no component may be a link, ".", ".." or empty), checks ownership with
 * fstat on each descriptor it relies on, validates the Meson marker through a
 * descriptor, and creates snapshots/run-XXXXXX with mkdtempat_np and its own
 * home, config, state, cache, runtime and output directories with mkdirat,
 * all relative to those descriptors, never by re-resolving a pathname. Every
 * pathname it composes is checked for truncation. It then points HOME,
 * CFFIXED_USER_HOME and XDG_* there and verifies Foundation sees that home.
 * It never re-executes, accepts no directory, token or environment from a
 * caller, and creates every file it writes exclusively without following
 * links, relative to the output or config directory descriptor.
 *
 * Residual limitation: the paths handed to Foundation and AppKit (HOME,
 * CFFIXED_USER_HOME, XDG_* and anything the page controllers derive from
 * them) are re-resolved by those frameworks and cannot be anchored to these
 * descriptors. They point inside a run directory this process just created
 * with mode 0700 under a directory owned by the user, which is what limits
 * the exposure, not descriptor anchoring. */
static const char *const FixtureDirs[] = {"home", "config", "state", "cache", "runtime", "out"};
static int Join(char *out, size_t size, const char *a, const char *b) {
    int n = snprintf(out, size, "%s/%s", a, b);
    return n > 0 && (size_t)n < size;
}
static int OwnedFD(int fd, mode_t type) {
    struct stat st;
    return fd >= 0 && !fstat(fd, &st) && (st.st_mode & S_IFMT) == type && st.st_uid == geteuid();
}
static int OpenDirAt(int at, const char *name) {
    return openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}
/* Opens an absolute canonical directory path without following any link. */
static int OpenCanonical(const char *path) {
    if (path[0] != '/' || !path[1]) return -1;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const char *p = path + 1; fd >= 0 && *p;) {
        size_t n = strcspn(p, "/");
        char name[NAME_MAX + 1];
        int bad = n == 0 || n > NAME_MAX || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.') ||
                  (p[n] == '/' && !p[n + 1]);
        int next = -1;
        if (!bad) { memcpy(name, p, n); name[n] = 0; next = OpenDirAt(fd, name); }
        close(fd); fd = next;
        p += n; if (*p == '/') ++p;
    }
    return fd;
}
static int Close(int fd, int result) { if (fd >= 0) close(fd); return result; }
static int Prepare(const char *root, char run[PATH_MAX]) {
    char path[PATH_MAX], name[] = "run-XXXXXX";
    /* Compose every pathname used below from the template first and refuse,
     * before creating anything, if any of them would be truncated. */
    if (strnlen(root, PATH_MAX) >= PATH_MAX || !Join(path, sizeof path, root, "snapshots") || !Join(run, PATH_MAX, path, name)) return 0;
    for (size_t i = 0; i < sizeof FixtureDirs / sizeof *FixtureDirs; ++i)
        if (!Join(path, sizeof path, run, FixtureDirs[i])) return 0;
    int rootfd = OpenCanonical(root);
    if (!OwnedFD(rootfd, S_IFDIR)) return Close(rootfd, 0);
    int meson = OpenDirAt(rootfd, "meson-private");
    /* O_NONBLOCK: a FIFO (or device) in the marker's place must not block
     * the open; its type is checked with fstat before anything else. */
    int marker = OwnedFD(meson, S_IFDIR) ? openat(meson, "coredata.dat", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) : -1;
    int ok = OwnedFD(marker, S_IFREG);
    Close(marker, 0); Close(meson, 0);
    if (!ok) return Close(rootfd, 0);
    if (mkdirat(rootfd, "snapshots", 0700) && errno != EEXIST) return Close(rootfd, 0);
    int snapshots = OpenDirAt(rootfd, "snapshots");
    Close(rootfd, 0);
    if (!OwnedFD(snapshots, S_IFDIR) || !mkdtempat_np(snapshots, name)) return Close(snapshots, 0);
    int runfd = OpenDirAt(snapshots, name);
    Close(snapshots, 0);
    /* Same length as the template checked above, now with the real name. */
    if (!OwnedFD(runfd, S_IFDIR) || !Join(path, sizeof path, root, "snapshots") || !Join(run, PATH_MAX, path, name))
        return Close(runfd, 0);
    for (size_t i = 0; i < sizeof FixtureDirs / sizeof *FixtureDirs; ++i) {
        if (mkdirat(runfd, FixtureDirs[i], 0700)) return Close(runfd, 0);
        int fd = OpenDirAt(runfd, FixtureDirs[i]);
        if (!OwnedFD(fd, S_IFDIR)) { Close(fd, 0); return Close(runfd, 0); }
        if (!strcmp(FixtureDirs[i], "out")) OutFD = fd;
        else if (!strcmp(FixtureDirs[i], "config")) ConfigFD = fd;
        else close(fd);
    }
    Close(runfd, 0);
    static const char *const names[] = {"HOME", "CFFIXED_USER_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"};
    static const char *const dirs[] = {"home", "home", "config", "state", "cache", "runtime"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i)
        if (!Join(path, sizeof path, run, dirs[i]) || setenv(names[i], path, 1)) return 0;
    return 1;
}
static NSString *Canonical(NSString *path) {
    char resolved[PATH_MAX];
    return path.isAbsolutePath && realpath(path.fileSystemRepresentation, resolved) ? @(resolved) : nil;
}

/* Entry point. See "Fixture-root safety" above, including its residual
 * limitation: paths handed to Foundation and AppKit are re-resolved by them
 * and cannot be descriptor-anchored. */
int main(int argc, char **argv) {
    char run[PATH_MAX];
    if (argc != 2) { fputs("usage: macos_manager_snapshots MESON_BUILD_DIR\n", stderr); return 2; }
    if (!Prepare(argv[1], run)) { fprintf(stderr, "refusing: %s is not a canonical meson build directory owned by you\n", argv[1]); return 2; }
    @autoreleasepool {
        NSString *home = [@(run) stringByAppendingPathComponent:@"home"];
        /* Foundation may report /private/... as /..., so compare canonically. */
        if (![Canonical(NSHomeDirectory()) isEqual:home]) { fputs("refusing: Foundation does not see the fixture home\n", stderr); return 2; }
        Out = [@(run) stringByAppendingPathComponent:@"out"];
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (!session || CGMainDisplayID() == kCGNullDirectDisplay) {
            if (session) CFRelease(session);
            puts("SKIP: no WindowServer session for offscreen rendering"); return 77;
        }
        CFRelease(session);
        [NSApplication.sharedApplication setActivationPolicy:NSApplicationActivationPolicyProhibited];
        Written = [NSMutableArray array];
        SeedAccounts(nil);
        Pages(NSAppearanceNameAqua, @"light");
        Pages(NSAppearanceNameDarkAqua, @"dark");
        NSMutableString *dump = [NSMutableString string];
        DumpMenu([MDManagerApp makeMainMenu], dump, 0);
        if (!WriteNew(@"menu.txt", [dump dataUsingEncoding:NSUTF8StringEncoding])) ++Failures;
        else [Written addObject:@"menu.txt"];
        /* Every expected artifact must exist in this fresh directory. */
        NSMutableArray *files = [NSMutableArray array];
        DIR *listing = fdopendir(dup(OutFD)); /* read through the descriptor */
        for (struct dirent *e; listing && (e = readdir(listing));)
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) [files addObject:@(e->d_name)];
        if (listing) closedir(listing); else ++Failures;
        /* Per appearance: 2 disconnected + 10 sections + 1 connecting + 17 sheets
         * + 2 This Computer states + 13 Hosts states + 2 minimum Hosts states. */
        const NSUInteger expected = 2 * (2 + 10 + 1 + 17 + 2 + 13 + 2) + 1;
        if (files.count != Written.count || files.count != expected) { fprintf(stderr, "FAIL: %lu files, %lu written\n", (unsigned long)files.count, (unsigned long)Written.count); ++Failures; }
        printf("wrote %lu files to %s\n", (unsigned long)Written.count, Out.UTF8String);
        return Failures ? 1 : 0;
    }
}
