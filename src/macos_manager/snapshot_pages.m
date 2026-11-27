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

#define KEY1 "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define KEY2 "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
#define KEY3 "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9"
#define KEY4 "e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd13"
#define GEN "abababababababababababababababab"
#define REQUEST "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"

static NSString *Out;
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
    NSString *path = [Out stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"png"]];
    if ([NSFileManager.defaultManager fileExistsAtPath:path]) { fprintf(stderr, "FAIL: duplicate %s\n", name.UTF8String); ++Failures; return; }
    if (!png || ![png writeToFile:path atomically:NO]) { fprintf(stderr, "FAIL: could not write %s\n", path.UTF8String); ++Failures; return; }
    [Written addObject:path.lastPathComponent];
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
    NSString *config = [@(getenv("XDG_CONFIG_HOME")) stringByAppendingPathComponent:@"metadesk"];
    mkdir(config.fileSystemRepresentation, 0700);
    NSString *prefs = [config stringByAppendingPathComponent:@"manager-relays.ini"];
    int fd = open(prefs.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { fputs("FAIL: fixture relay settings already exist\n", stderr); ++Failures; }
    else {
        static const char corrupt[] = "not a relay preferences file";
        BOOL wrote = write(fd, corrupt, sizeof corrupt - 1) == (ssize_t)(sizeof corrupt - 1);
        close(fd);
        if (wrote) Try(s, n(@"sheet-hosts-unreadable-relays-"), hosts, @selector(refresh:));
        unlink(prefs.fileSystemRepresentation);
    }
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
    [c.local setValue:@YES forKey:@"noHostRole"];
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

/* ---- Fixture-root safety (md-b9zo B3) ----
 * The renderer only ever reads and writes inside a run directory it creates
 * itself with mkdtemp under BUILD/snapshots, where BUILD is a canonical meson
 * build directory owned by this user. It then re-executes itself with HOME,
 * CFFIXED_USER_HOME and XDG_* pointing into that run directory and refuses to
 * render unless every one of them, and NSHomeDirectory(), is exactly that. */
static NSString *Canonical(NSString *path) {
    char resolved[PATH_MAX];
    return path.isAbsolutePath && realpath(path.fileSystemRepresentation, resolved) ? @(resolved) : nil;
}
static BOOL OwnedDirectory(NSString *path) {
    struct stat st;
    return !lstat(path.fileSystemRepresentation, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid();
}
static BOOL BuildRoot(NSString *root) {
    struct stat st;
    NSString *marker = [root stringByAppendingPathComponent:@"meson-private/coredata.dat"];
    return [Canonical(root) isEqual:root] && OwnedDirectory(root) &&
        !lstat(marker.fileSystemRepresentation, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid();
}
static NSArray<NSString *> *FixtureDirectories(void) { return @[@"home", @"config", @"state", @"cache", @"out"]; }
static int Launch(const char *self, NSString *root) {
    if (!BuildRoot(root)) { fprintf(stderr, "refusing: %s is not a canonical meson build directory owned by you\n", root.UTF8String); return 2; }
    NSString *parent = [root stringByAppendingPathComponent:@"snapshots"];
    if (mkdir(parent.fileSystemRepresentation, 0700) && errno != EEXIST) return 2;
    if (!OwnedDirectory(parent) || ![Canonical(parent) isEqual:parent]) { fputs("refusing: snapshots is not a plain directory\n", stderr); return 2; }
    char run[PATH_MAX];
    snprintf(run, sizeof run, "%s/run-XXXXXX", parent.fileSystemRepresentation);
    if (!mkdtemp(run)) return 2;
    NSString *dir = @(run);
    for (NSString *sub in FixtureDirectories())
        if (mkdir([dir stringByAppendingPathComponent:sub].fileSystemRepresentation, 0700)) return 2;
    NSString *token = NSUUID.UUID.UUIDString;
    int fd = open([dir stringByAppendingPathComponent:@".fixture-token"].fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0 || write(fd, token.UTF8String, token.length) != (ssize_t)token.length) return 2;
    close(fd);
    setenv("HOME", [dir stringByAppendingPathComponent:@"home"].fileSystemRepresentation, 1);
    setenv("CFFIXED_USER_HOME", [dir stringByAppendingPathComponent:@"home"].fileSystemRepresentation, 1);
    setenv("XDG_CONFIG_HOME", [dir stringByAppendingPathComponent:@"config"].fileSystemRepresentation, 1);
    setenv("XDG_STATE_HOME", [dir stringByAppendingPathComponent:@"state"].fileSystemRepresentation, 1);
    setenv("XDG_CACHE_HOME", [dir stringByAppendingPathComponent:@"cache"].fileSystemRepresentation, 1);
    unsetenv("XDG_RUNTIME_DIR");
    char *args[] = {(char *)self, "--fixture-run", run, (char *)token.UTF8String, NULL};
    execv(self, args);
    perror("execv"); return 2;
}
static NSString *CheckFixture(NSString *run, NSString *token) {
    NSString *canonical = Canonical(run);
    if (![canonical isEqual:run] || !OwnedDirectory(run)) return nil;
    NSString *snapshots = run.stringByDeletingLastPathComponent;
    if (![snapshots.lastPathComponent isEqual:@"snapshots"] || !BuildRoot(snapshots.stringByDeletingLastPathComponent) ||
        ![run.lastPathComponent hasPrefix:@"run-"]) return nil;
    NSData *stored = [NSData dataWithContentsOfFile:[run stringByAppendingPathComponent:@".fixture-token"] options:NSDataReadingUncached error:NULL];
    if (![[[NSString alloc] initWithData:stored encoding:NSUTF8StringEncoding] isEqual:token]) return nil;
    NSDictionary *want = @{@"HOME":@"home", @"CFFIXED_USER_HOME":@"home", @"XDG_CONFIG_HOME":@"config", @"XDG_STATE_HOME":@"state", @"XDG_CACHE_HOME":@"cache"};
    for (NSString *name in want) {
        const char *value = getenv(name.UTF8String);
        NSString *expected = [run stringByAppendingPathComponent:want[name]];
        if (!value || ![@(value) isEqual:expected] || ![Canonical(expected) isEqual:expected] || !OwnedDirectory(expected)) return nil;
    }
    /* Foundation may report /private/tmp/... as /tmp/...; compare canonically. */
    if (![Canonical(NSHomeDirectory()) isEqual:[run stringByAppendingPathComponent:@"home"]]) return nil;
    return [run stringByAppendingPathComponent:@"out"];
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc == 2) return Launch(argv[0][0] == '/' ? argv[0] : realpath(argv[0], NULL), @(argv[1]));
        if (argc != 4 || strcmp(argv[1], "--fixture-run")) { fputs("usage: macos_manager_snapshots MESON_BUILD_DIR\n", stderr); return 2; }
        Out = CheckFixture(@(argv[2]), @(argv[3]));
        if (!Out) { fputs("refusing: not a fixture run directory created by this renderer\n", stderr); return 2; }
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
        NSString *menu = [Out stringByAppendingPathComponent:@"menu.txt"];
        if (![dump writeToFile:menu atomically:NO encoding:NSUTF8StringEncoding error:NULL]) ++Failures;
        else [Written addObject:@"menu.txt"];
        /* Every expected artifact must exist in this fresh directory. */
        NSArray *files = [NSFileManager.defaultManager contentsOfDirectoryAtPath:Out error:NULL];
        /* Per appearance: 2 disconnected + 10 sections + 1 connecting + 17 sheets
         * + 2 This Computer states + 13 Hosts states + 2 minimum Hosts states. */
        const NSUInteger expected = 2 * (2 + 10 + 1 + 17 + 2 + 13 + 2) + 1;
        if (files.count != Written.count || files.count != expected) { fprintf(stderr, "FAIL: %lu files, %lu written\n", (unsigned long)files.count, (unsigned long)Written.count); ++Failures; }
        printf("wrote %lu files to %s\n", (unsigned long)Written.count, Out.UTF8String);
        return Failures ? 1 : 0;
    }
}
