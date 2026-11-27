/* Test-only offscreen renderer for design review (md-b9zo).
 *
 * Builds the real page controllers with fixture workers, lays the main window
 * out at its default and minimum sizes in light and dark appearance, opens each
 * sheet, and renders everything to PNG with cacheDisplayInRect:. No window is
 * ordered on screen, nothing is activated, no pasteboard, Keychain, signer,
 * relay or host IPC is used. Run it through snapshot_pages.py, which gives it a
 * private HOME, CFFIXED_USER_HOME and XDG directories. */
#import "manager_app.h"
#import "hosts_page.h"
#import "hosts_model.h"
#import "access_page.h"
#import "identity_page.h"
#include <stdatomic.h>

#define KEY1 "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define KEY2 "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
#define KEY3 "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9"
#define KEY4 "e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd13"
#define GEN "abababababababababababababababab"
#define REQUEST "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"

static NSString *Out;
static int Failures;
static NSUInteger LabelCount(NSView *root);
static NSString *Slug(NSString *section);
static BOOL LabelContainsText(NSView *root, NSString *text);
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
    if (!png || ![png writeToFile:path atomically:YES]) { fprintf(stderr, "FAIL: could not write %s\n", path.UTF8String); ++Failures; return; }
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
- (NSArray *)rows { return self.empty ? @[] : SnapRows(); }
- (NSArray *)quick { return self.empty ? @[] : @[SnapRows()[0], SnapRows()[3], SnapRows()[1]]; }
- (NSArray *)tags { return self.empty ? @[] : @[@{@"id":@"t2", @"label":@"Linux"}, @{@"id":@"t1", @"label":@"Office"}]; }
- (NSArray *)issues {
    return self.empty ? @[] : @[@{@"coordinate":@"30078:" KEY1 ":metadesk-hosts", @"message":@"Your edit conflicts with a newer remote version.",
        @"conflict":@YES, @"retry":@NO, @"ready":@YES}];
}
- (NSString *)status { return self.empty ? @"Offline — showing cached hosts. Add explicit bootstrap relays to sync." : @"Address book synced."; }
- (NSString *)owner { return @KEY1; }
- (BOOL)canReview { return !self.empty; }
- (BOOL)canEdit { return !self.empty; }
- (BOOL)canExport { return !self.empty; }
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
@end
@implementation SnapHosts
- (BOOL)openDirectory:(NSString *)directory error:(NSString **)error { (void)directory; (void)error; return YES; }
- (MDHostsBook *)snapshot { SnapBook *b = [SnapBook new]; b.empty = self.empty; return b; }
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
                 SnapHosts *worker; SnapLink *link; NSWindow *window; NSTabView *tabs; } Scene;

/* The pre-md-b9zo app had no window factory; rebuild its tab view exactly as
 * applicationDidFinishLaunching: did. */
static NSWindow *LegacyWindow(Scene *s) {
    NSTabView *tabs = [NSTabView new];
    NSArray *pages = @[@[@"Hosts", s->hosts.view], @[@"This Computer", s->local.computerView], @[@"Access", s->local.accessView],
                       @[@"Identity", s->identity.view], @[@"Relays", [s->app performSelector:@selector(relays)]]];
    for (NSArray *p in pages) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:p[0]];
        item.label = p[0]; item.view = p[1]; [tabs addTabViewItem:item];
    }
    NSView *activity = [NSView new];
    NSTextField *note = [NSTextField labelWithString:@"Live session status and disconnection are in This Computer. No activity history is stored here."];
    note.translatesAutoresizingMaskIntoConstraints = NO; [activity addSubview:note];
    [note.leadingAnchor constraintEqualToAnchor:activity.leadingAnchor constant:14].active = YES;
    [note.topAnchor constraintEqualToAnchor:activity.topAnchor constant:14].active = YES;
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"Activity"];
    item.label = @"Activity"; item.view = activity; [tabs addTabViewItem:item];
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 820, 540)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    w.title = @"Metadesk Manager"; w.contentMinSize = NSMakeSize(650, 410); w.contentView = tabs;
    s->tabs = tabs;
    return w;
}
static NSArray<NSString *> *Sections(Scene *s) {
    if ([s->app respondsToSelector:@selector(sectionIdentifiers)]) return [s->app performSelector:@selector(sectionIdentifiers)];
    return [s->tabs.tabViewItems valueForKey:@"identifier"];
}
static void ShowSection(Scene *s, NSString *identifier) {
    if (s->tabs) [s->tabs selectTabViewItemWithIdentifier:identifier];
    else [s->app performSelector:@selector(showSection:) withObject:identifier];
    if ([identifier isEqual:@"Hosts"]) [s->hosts activate];
    if ([identifier isEqual:@"Identity"]) [s->identity activate];
    Spin();
}
static Scene MakeScene(NSString *accounts, NSAppearanceName appearance, BOOL emptyHosts) {
    Scene s = {0};
    s.worker = [[SnapHosts alloc] initWithChange:nil]; s.worker.empty = emptyHosts;
    s.hosts = [[MDHostsPage alloc] initWithWorker:s.worker];
    SnapLink *link = [[SnapLink alloc] initWithUpdate:nil]; s.link = link;
    s.local = [[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) { link.push = update; return link; }];
    s.identity = [[MDIdentityPage alloc] initWithHosts:s.hosts directory:accounts connect:SnapConnect credentials:^{ return SnapStore(); }];
    s.app = [[MDManagerApp alloc] initWithHosts:s.hosts local:s.local identity:s.identity];
    s.window = [s.app respondsToSelector:@selector(makeMainWindow)] ? [s.app performSelector:@selector(makeMainWindow)] : LegacyWindow(&s);
    s.window.releasedWhenClosed = NO; s.window.restorable = NO; s.window.alphaValue = 0;
    s.window.appearance = [NSAppearance appearanceNamed:appearance];
    return s;
}
static void Resize(NSWindow *window, NSSize content) {
    NSRect frame = [window frameRectForContentRect:(NSRect){NSZeroPoint, content}];
    [window setFrame:frame display:NO];
    [window layoutIfNeeded];
}
static NSSize DefaultSize(Scene *s) {
    if ([s->app respondsToSelector:@selector(defaultContentSize)]) {
        NSValue *v = [s->app performSelector:@selector(defaultContentSize)]; return v.sizeValue;
    }
    return NSMakeSize(820, 540);
}
static void FeedAccess(Scene *s, BOOL connected) {
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    if (connected) s->link.push(@"Connected to the local host.", AccessSnapshot(now), AccessPolicy(),
                                @{@"screen_recording":@YES, @"accessibility":@NO}, NO);
    Spin();
}

static void CaptureSheet(Scene *s, NSString *name, void (^open)(void)) {
    NSWindow *window = s->window;
    open();
    if (!Await(^BOOL{ return window.attachedSheet != nil; })) {
        fprintf(stderr, "note: %s did not open a sheet\n", name.UTF8String); return;
    }
    NSWindow *sheet = window.attachedSheet;
    Spin();
    [sheet layoutIfNeeded];
    Render(Frame(sheet), name);
    [window endSheet:sheet returnCode:NSModalResponseCancel];
    Await(^BOOL{ return window.attachedSheet == nil; });
    Spin();
}
static void Try(Scene *s, NSString *name, id target, SEL action) {
    if (![target respondsToSelector:action]) { fprintf(stderr, "note: %s has no %s\n", name.UTF8String, sel_getName(action)); return; }
    CaptureSheet(s, name, ^{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        [target performSelector:action withObject:nil];
#pragma clang diagnostic pop
    });
}
static NSTableView *FirstTable(NSView *root) {
    return (NSTableView *)Find(root, ^BOOL(NSView *v) { return [v isKindOfClass:NSTableView.class] && ![v isKindOfClass:NSOutlineView.class]
        && [((NSTableView *)v).tableColumns.firstObject.identifier isEqual:@"Host"] ; });
}
static NSTableView *HostsTable(NSView *root) {
    NSTableView *t = FirstTable(root);
    return t ?: (NSTableView *)Find(root, ^BOOL(NSView *v) { return [v isKindOfClass:NSTableView.class] && [v.accessibilityLabel isEqual:@"Saved hosts"]; });
}

static void Sheets(Scene *s, NSString *suffix) {
    MDHostsPage *hosts = s->hosts;
    ShowSection(s, @"Hosts");
    Await(^BOOL{ return HostsTable(hosts.view).numberOfRows > 0; });
    NSTableView *table = HostsTable(hosts.view);
    [table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    Spin();
    SnapReview *first = [SnapReview new], *changed = [SnapReview new]; changed.mismatch = YES;
    CaptureSheet(s, [@"sheet-hosts-review-" stringByAppendingString:suffix], ^{ [hosts performSelector:@selector(presentReview:) withObject:first]; });
    CaptureSheet(s, [@"sheet-hosts-key-changed-" stringByAppendingString:suffix], ^{ [hosts performSelector:@selector(presentReview:) withObject:changed]; });
    Try(s, [@"sheet-hosts-edit-" stringByAppendingString:suffix], hosts, @selector(edit:));
    NSArray *imports = @[
        @{@"key":@KEY3, @"npub":MDHostNpub(@KEY3), @"fingerprint":MDHostFingerprint(@KEY3), @"label":@"Lab iMac", @"alias":@"lab@example.invalid", @"caps":@"Video (view only)"},
        @{@"key":@KEY4, @"npub":MDHostNpub(@KEY4), @"fingerprint":MDHostFingerprint(@KEY4), @"label":@"Kiosk", @"alias":@"", @"caps":@"Video, input"}];
    CaptureSheet(s, [@"sheet-hosts-import-" stringByAppendingString:suffix], ^{
        [hosts performSelector:@selector(reviewImport:book:) withObject:imports withObject:[s->worker snapshot]];
    });
    Try(s, [@"sheet-hosts-export-" stringByAppendingString:suffix], hosts, @selector(exportHosts:));
    Try(s, [@"sheet-hosts-conflict-" stringByAppendingString:suffix], hosts, @selector(handleIssue:));
    Try(s, [@"sheet-hosts-relays-" stringByAppendingString:suffix], hosts, @selector(editRelays:));
    Try(s, [@"sheet-hosts-connect-" stringByAppendingString:suffix], hosts, @selector(showConnectSheet:));
    /* Unreadable relay preferences ask before replacement. */
    NSString *config = [@(getenv("XDG_CONFIG_HOME")) stringByAppendingPathComponent:@"metadesk"];
    [NSFileManager.defaultManager createDirectoryAtPath:config withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:NULL];
    NSString *prefs = [config stringByAppendingPathComponent:@"manager-relays.ini"];
    [@"not a relay preferences file" writeToFile:prefs atomically:NO encoding:NSUTF8StringEncoding error:NULL];
    Try(s, [@"sheet-hosts-unreadable-relays-" stringByAppendingString:suffix], hosts, @selector(refresh:));
    [NSFileManager.defaultManager removeItemAtPath:prefs error:NULL];
    Spin();

    ShowSection(s, @"Access");
    FeedAccess(s, YES);
    Try(s, [@"sheet-access-review-" stringByAppendingString:suffix], s->local, @selector(review:));
    Try(s, [@"sheet-access-edit-" stringByAppendingString:suffix], s->local, @selector(edit:));
    Try(s, [@"sheet-access-revoke-" stringByAppendingString:suffix], s->local, @selector(revoke:));
    Try(s, [@"sheet-access-kick-revoke-" stringByAppendingString:suffix], s->local, @selector(kickRevoke:));
    ShowSection(s, @"This Computer");
    Try(s, [@"sheet-computer-disconnect-" stringByAppendingString:suffix], s->local, @selector(kick:));
    Try(s, [@"sheet-computer-qr-" stringByAppendingString:suffix], s->local, @selector(showQR:));

    ShowSection(s, @"Identity");
    Await(^BOOL{ return LabelCount(s->identity.view) > 0; });
    Try(s, [@"sheet-identity-add-" stringByAppendingString:suffix], s->identity, @selector(add:));
    Try(s, [@"sheet-identity-label-" stringByAppendingString:suffix], s->identity, @selector(editLabel:));
    Try(s, [@"sheet-identity-pairing-" stringByAppendingString:suffix], s->identity, @selector(importPairing:));
    Try(s, [@"sheet-identity-remove-" stringByAppendingString:suffix], s->identity, @selector(remove:));
}
static NSUInteger LabelCount(NSView *root) {
    __block NSUInteger n = 0;
    Find(root, ^BOOL(NSView *v) {
        if ([v isKindOfClass:NSPopUpButton.class] && [((NSPopUpButton *)v).titleOfSelectedItem length] &&
            ![((NSPopUpButton *)v).titleOfSelectedItem isEqual:@"No saved accounts"]) ++n;
        if ([v isKindOfClass:NSTableView.class]) n += (NSUInteger)((NSTableView *)v).numberOfRows;
        return NO;
    });
    return n;
}

static void Pages(NSString *accounts, NSAppearanceName appearance, NSString *mode) {
    Scene s = MakeScene(accounts, appearance, NO);
    NSSize size = DefaultSize(&s);
    NSSize sizes[2] = {size, s.window.contentMinSize};
    NSString *names[2] = {@"default", @"minimum"};
    /* Offline/disconnected first, then with fixture state. */
    Resize(s.window, sizes[0]);
    for (NSString *section in @[@"This Computer", @"Access"]) {
        ShowSection(&s, section);
        Render(Frame(s.window), [NSString stringWithFormat:@"page-%@-disconnected-%@-%@", Slug(section), names[0], mode]);
    }
    for (int z = 0; z < 2; ++z) {
        Resize(s.window, sizes[z]);
        for (NSString *section in Sections(&s)) {
            ShowSection(&s, section);
            if ([section isEqual:@"Hosts"]) Await(^BOOL{ return HostsTable(s.hosts.view).numberOfRows > 0; });
            if ([section isEqual:@"Identity"]) Await(^BOOL{ return LabelCount(s.identity.view) > 0; });
            if ([section isEqual:@"This Computer"] || [section isEqual:@"Access"]) FeedAccess(&s, YES);
            if ([section isEqual:@"Hosts"]) {
                NSTableView *t = HostsTable(s.hosts.view);
                if (t.numberOfRows) [t selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
                if (getenv("MD_SNAPSHOT_DEBUG")) for (NSTableColumn *c in t.tableColumns) fprintf(stderr, "col %s w=%.0f min=%.0f mask=%lu\n", c.identifier.UTF8String, c.width, c.minWidth, (unsigned long)c.resizingMask);
                Spin();
            }
            Render(Frame(s.window), [NSString stringWithFormat:@"page-%@-%@-%@", Slug(section), names[z], mode]);
        }
    }
    NSTableView *side = (NSTableView *)Find(Frame(s.window), ^BOOL(NSView *v) { return [v isKindOfClass:NSTableView.class] && [v.accessibilityLabel isEqual:@"Sections"]; });
    if (getenv("MD_SNAPSHOT_DEBUG")) for (NSView *v = side; v; v = v.superview) fprintf(stderr, "  %s %s hidden=%d layer=%d\n", NSStringFromClass(v.class).UTF8String, NSStringFromRect(v.frame).UTF8String, v.isHidden, v.wantsLayer);
    if (getenv("MD_SNAPSHOT_DEBUG")) fprintf(stderr, "sidebar rows=%ld frame=%s superview=%s\n", (long)side.numberOfRows, NSStringFromRect(side.frame).UTF8String, NSStringFromRect(side.enclosingScrollView.frame).UTF8String);
    Resize(s.window, size);
    Sheets(&s, mode);
    [s.window close];
    /* Empty and offline Hosts state. */
    Scene e = MakeScene(accounts, appearance, YES);
    Resize(e.window, DefaultSize(&e));
    ShowSection(&e, @"Hosts");
    Await(^BOOL{ return LabelContainsText(e.hosts.view, @"Offline"); });
    Spin();
    Render(Frame(e.window), [NSString stringWithFormat:@"page-hosts-empty-offline-default-%@", mode]);
    [e.window close];
}
static NSString *Slug(NSString *section) { return [[section lowercaseString] stringByReplacingOccurrencesOfString:@" " withString:@"-"]; }
static BOOL LabelContainsText(NSView *root, NSString *text) {
    return Find(root, ^BOOL(NSView *v) { return [v isKindOfClass:NSTextField.class] && [((NSTextField *)v).stringValue containsString:text]; }) != nil;
}

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
            BOOL upper = [item.keyEquivalent isEqual:item.keyEquivalent.uppercaseString] && ![item.keyEquivalent isEqual:item.keyEquivalent.lowercaseString];
            if ((m & NSEventModifierFlagShift) || upper) [key appendString:@"⇧"];
            if (m & NSEventModifierFlagCommand) [key appendString:@"⌘"];
            NSString *k = item.keyEquivalent;
            if ([k isEqual:@"\b"] || [k isEqual:@"\x7f"]) k = @"⌫";
            [key appendString:k.uppercaseString];
            [line appendFormat:@"\t%@", key];
        }
        if (item.action) [line appendFormat:@"\t[%@]", NSStringFromSelector(item.action)];
        [out appendFormat:@"%@\n", line];
        if (item.submenu) DumpMenu(item.submenu, out, depth + 1);
    }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) { fputs("usage: snapshot_pages ACCOUNTS_DIR OUTPUT_DIR\n", stderr); return 2; }
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (!session || CGMainDisplayID() == kCGNullDirectDisplay) {
            if (session) CFRelease(session);
            puts("SKIP: no WindowServer session for offscreen rendering"); return 77;
        }
        CFRelease(session);
        [NSApplication.sharedApplication setActivationPolicy:NSApplicationActivationPolicyProhibited];
        Out = @(argv[2]); Written = [NSMutableArray array];
        [NSFileManager.defaultManager createDirectoryAtPath:Out withIntermediateDirectories:YES attributes:nil error:NULL];
        NSString *accounts = @(argv[1]);
        SeedAccounts(accounts);
        Pages(accounts, NSAppearanceNameAqua, @"light");
        Pages(accounts, NSAppearanceNameDarkAqua, @"dark");
        if ([MDManagerApp respondsToSelector:@selector(makeMainMenu)]) {
            NSMutableString *dump = [NSMutableString string];
            DumpMenu([MDManagerApp performSelector:@selector(makeMainMenu)], dump, 0);
            [dump writeToFile:[Out stringByAppendingPathComponent:@"menu.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
            [Written addObject:@"menu.txt"];
        }
        printf("wrote %lu files to %s\n", (unsigned long)Written.count, Out.UTF8String);
        return Failures ? 1 : 0;
    }
}
