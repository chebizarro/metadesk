#import "access_page.h"
#import "access_model.h"
#import "hosts_model.h"
#import "host_qr.h"
#import "model.h"
#import "ui.h"
#include "accounts.h"
#import <Security/Security.h>
#import <UserNotifications/UserNotifications.h>

static NSTimeInterval Now(void) { return NSDate.date.timeIntervalSince1970; }
static BOOL DistributionSigned(void) {
    if (![NSBundle.mainBundle.bundlePath hasSuffix:@".app"] || !NSBundle.mainBundle.bundleIdentifier) return NO;
    SecCodeRef code = NULL; SecStaticCodeRef stat = NULL; CFDictionaryRef info = NULL;
    BOOL ok = SecCodeCopySelf(kSecCSDefaultFlags, &code) == errSecSuccess &&
        SecCodeCopyStaticCode(code, kSecCSDefaultFlags, &stat) == errSecSuccess &&
        SecStaticCodeCheckValidity(stat, kSecCSDefaultFlags, NULL) == errSecSuccess &&
        SecCodeCopySigningInformation(stat, kSecCSSigningInformation, &info) == errSecSuccess &&
        [((__bridge NSDictionary *)info)[(__bridge NSString *)kSecCodeInfoTeamIdentifier] length] > 0;
    if (info) CFRelease(info); if (stat) CFRelease(stat); if (code) CFRelease(code);
    return ok;
}

/* Permission checkboxes for an approval or an edit. Disabled boxes can't be
 * granted: the client didn't request them or the host doesn't support them. */
@interface MDCapChooser : NSObject
@property(nonatomic) NSArray<NSButton *> *choices;
@property(nonatomic) MDStatusView *lifetime;
@property(nonatomic) MDSheet *sheet;
@property(nonatomic, copy) dispatch_block_t update;
- (NSView *)viewWithAllowed:(uint32_t)allowed initial:(uint32_t)initial;
- (uint32_t)selected;
@end
@implementation MDCapChooser
static const uint32_t Caps[] = {MD_CAP_VIDEO, MD_CAP_AGENT, MD_CAP_INPUT, MD_CAP_MONITOR, MD_CAP_CLIPBOARD};
- (NSView *)viewWithAllowed:(uint32_t)allowed initial:(uint32_t)initial {
    NSMutableArray *list = [NSMutableArray array];
    for (NSUInteger i=0;i<sizeof(Caps)/sizeof(*Caps);++i) {
        NSButton *b = [NSButton checkboxWithTitle:MDAccessCaps(Caps[i]) target:self action:@selector(changed:)];
        b.enabled = (allowed & Caps[i]) != 0; b.state = (initial & Caps[i]) ? NSControlStateValueOn : NSControlStateValueOff;
        [list addObject:b];
    }
    self.choices = [list copy];
    NSStackView *boxes = [NSStackView stackViewWithViews:list];
    boxes.orientation = NSUserInterfaceLayoutOrientationVertical; boxes.alignment = NSLayoutAttributeLeading; boxes.spacing = 6;
    return boxes;
}
- (void)changed:(id)sender { (void)sender; if (self.update) self.update(); }
- (uint32_t)selected {
    uint32_t mask = 0;
    for (NSUInteger i=0;i<self.choices.count;++i) if (self.choices[i].state == NSControlStateValueOn) mask |= Caps[i];
    return mask;
}
@end

@interface MDLocalHostPages () <UNUserNotificationCenterDelegate, NSTableViewDataSource, NSTableViewDelegate> {
    MDHostLink *_link;
    dispatch_group_t _qrWork;
    NSButton *_showQR;
    NSDictionary *_snapshot, *_policy, *_permissions;
    BOOL _busy, _closed;
    NSTextField *_endpoint, *_agent, *_session, *_sessionKey, *_notificationStatus, *_lifecycle;
    NSButton *_retry, *_openIdentity, *_advanced;
    NSView *_customRow;
    NSString *_linkPath;             /* what the link was last asked to connect to */
    NSUInteger _discovery;           /* fences discovery completions from an earlier role */
    NSUInteger _linkEpoch;           /* fences decisions made against an earlier link */
    NSString *_hostKey;              /* host-role key the last completed lookup found */
    NSString *_roleKey;              /* latest host-role key Identity reported (nil: unknown) */
    NSWindow *_decision;             /* open approval, edit or confirmation sheet */
    dispatch_queue_t _discoveryQueue; /* serial: requests are answered in order */
    BOOL _located, _noHostRole, _activated, _custom, _locating;
    id _policyGeneration;            /* remembered clients loaded for this generation */
    MDStatusView *_status, *_accessStatus, *_screen, *_ax;
    NSTableView *_requests, *_clients;
    NSArray<NSDictionary *> *_requestRows, *_clientRows;
    NSString *_selectedToken, *_selectedPeer;
    NSView *_requestsEmpty, *_clientsEmpty;
    NSTextField *_requestsEmptyTitle, *_requestsEmptyDetail, *_clientsEmptyTitle, *_clientsEmptyDetail;
    NSButton *_connect, *_recheck, *_kick, *_review, *_edit, *_revoke, *_kickRevoke;
    NSTimer *_clock;
    MDCapChooser *_chooser;
    NSUInteger _pendingCount;
    UNUserNotificationCenter *_notifications;
    NSMutableSet<NSString *> *_posted, *_noticeCooling;
    NSMutableDictionary<NSString *, NSNumber *> *_noticeAttempts;
}
@property(nonatomic, readwrite) NSView *computerView, *accessView;
@end

@implementation MDLocalHostPages
- (instancetype)init { return [self initWithLinkFactory:nil]; }
- (instancetype)initWithLinkFactory:(MDHostLink *(^)(MDHostUpdate))factory {
    if (!(self=[super init])) return nil;
    __weak MDLocalHostPages *weakSelf = self;
    MDHostUpdate update = ^(NSString *status, NSDictionary *s, NSDictionary *p, NSDictionary *perms, BOOL busy) {
        [weakSelf received:status snapshot:s policy:p permissions:perms busy:busy];
    };
    _link = factory ? factory(update) : [[MDHostLink alloc] initWithUpdate:update];
    _qrWork = dispatch_group_create();
    [self buildComputerView];
    [self buildAccessView];
    _posted = [NSMutableSet set]; _noticeCooling=[NSMutableSet set]; _noticeAttempts=[NSMutableDictionary dictionary];
    [self showStatus:nil];
    [self render];
    if (!factory) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        if (!DistributionSigned()) return;
        UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
        [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *setting) {
            if (setting.authorizationStatus != UNAuthorizationStatusAuthorized && setting.authorizationStatus != UNAuthorizationStatusProvisional) return;
            dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf enableNotifications:center]; });
        }];
    });
    return self;
}
static NSTableView *AccessTable(id owner, NSString *label) {
    MDTable *t = [MDTable new];
    t.dataSource = owner; t.delegate = owner; t.accessibilityLabel = label;
    t.allowsEmptySelection = YES; t.allowsMultipleSelection = NO;
    return t;
}
- (void)buildComputerView {
    _status = [MDStatusView new];
    _retry = MDButton(@"Try Again", self, @selector(reconnect:));
    _openIdentity = MDButton(@"Open Identity", self, @selector(openIdentity:));
    NSStackView *statusRow = MDRow(@[_status, _retry, _openIdentity]); statusRow.alignment = NSLayoutAttributeCenterY;
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _agent = MDWrappingValue(@""); _session = MDWrappingValue(@"");
    _sessionKey = MDValue(@""); _sessionKey.font = [NSFont monospacedSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
    _sessionKey.accessibilityLabel = @"Connected client’s fingerprint";
    _kick = MDButton(@"Disconnect Session…", self, @selector(kick:));
    _showQR = MDButton(@"Show Host QR Code…", self, @selector(showQR:));
    _showQR.toolTip = @"Show the saved host role’s public key as a QR code to scan from another device.";
    NSGridView *host = MDForm(@[@[NSNull.null, statusRow], @[@"Host", _agent], @[@"Session", _session],
                                @[@"Client", _sessionKey], @[NSNull.null, _kick], @[@"Identity", _showQR]]);
    /* A custom socket is an advanced case; the standard location is automatic. */
    _advanced = [NSButton buttonWithTitle:@"Use a custom management socket" target:self action:@selector(toggleAdvanced:)];
    _advanced.bezelStyle = NSBezelStyleDisclosure; _advanced.buttonType = NSButtonTypeOnOff; _advanced.title = @"";
    NSTextField *advancedLabel = MDLabel(@"Use a custom management socket");
    NSClickGestureRecognizer *click = [[NSClickGestureRecognizer alloc] initWithTarget:self action:@selector(toggleAdvancedLabel:)];
    [advancedLabel addGestureRecognizer:click];
    _advanced.accessibilityLabel = @"Use a custom management socket";
    NSStackView *advancedRow = MDRow(@[_advanced, advancedLabel]); advancedRow.alignment = NSLayoutAttributeCenterY; advancedRow.spacing = 4;
    _endpoint = MDKeyField(@"/absolute/path/to/control.sock", @"Management socket");
    _endpoint.target = self; _endpoint.action = @selector(connect:);
    _connect = MDButton(@"Connect", self, @selector(connect:));
    _customRow = MDRow(@[_endpoint, _connect]);
    [host addRowWithViews:@[NSGridCell.emptyContentView, advancedRow]];
    [host addRowWithViews:@[MDFormLabel(@"Socket"), _customRow]];
    [host rowAtIndex:host.numberOfRows - 1].hidden = YES;
    _screen = [MDStatusView new]; _ax = [MDStatusView new];
    NSButton *screen = MDButton(@"Open Settings", self, @selector(screen:));
    NSButton *ax = MDButton(@"Open Settings", self, @selector(ax:));
    screen.accessibilityLabel = @"Open Screen Recording settings"; ax.accessibilityLabel = @"Open Accessibility settings";
    _recheck = MDButton(@"Recheck Permissions", self, @selector(permissions:));
    NSStackView *screenRow = MDRow(@[_screen, screen]), *axRow = MDRow(@[_ax, ax]);
    screenRow.alignment = axRow.alignment = NSLayoutAttributeCenterY;
    [_screen setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_ax setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSGridView *permissions = MDForm(@[@[@"Screen Recording", screenRow], @[@"Accessibility", axRow], @[NSNull.null, _recheck]]);
    [permissions columnAtIndex:1].xPlacement = NSGridCellPlacementLeading;
    NSButton *help = [NSButton buttonWithTitle:@"" target:nil action:@selector(openHelp:)];
    help.bezelStyle = NSBezelStyleHelpButton; help.accessibilityLabel = @"Metadesk Manager Help";
    _lifecycle = MDNote(@"Metadesk Manager doesn’t start or stop the host; Help explains how to run it yourself.");
    NSStackView *footer = MDRow(@[_lifecycle, help]); footer.alignment = NSLayoutAttributeCenterY;
    NSStackView *stack = nil;
    NSView *page = MDPage(@[MDHeader(@"Local Host"), host, MDSeparator(),
        MDHeader(@"Permissions"),
        MDNote(@"The host needs these to share this Mac. Grant them to the host app in System Settings, not to Metadesk Manager. Checking doesn’t ask for permission or turn on sharing."),
        permissions, MDSeparator(), footer], &stack);
    for (NSView *v in stack.arrangedSubviews) if ([v isKindOfClass:NSBox.class]) {
        NSUInteger i = [stack.arrangedSubviews indexOfObject:v];
        [stack setCustomSpacing:MDGroupSpacing afterView:stack.arrangedSubviews[i - 1]];
        [stack setCustomSpacing:MDGroupSpacing afterView:v];
    }
    MDAlignForms(@[host, permissions]);
    self.computerView = MDScrollingPage(page);
}
- (void)toggleAdvancedLabel:(id)sender { (void)sender; _advanced.state = !_advanced.state; [self toggleAdvanced:_advanced]; }
- (void)toggleAdvanced:(id)sender {
    (void)sender;
    MDSetRowHidden(_customRow, _advanced.state != NSControlStateValueOn);
    if (_advanced.state == NSControlStateValueOn) [_endpoint.window makeFirstResponder:_endpoint];
}
- (void)openIdentity:(id)sender { (void)sender; [NSApp sendAction:@selector(showSection:) to:nil from:@"Identity"]; }
/* Connect automatically to the saved host role's standard socket. The path
 * comes from the same rule the host uses; the link authenticates the peer. */
- (void)activate {
    if (_closed || _activated) return;
    _activated = YES;
    [self locateAndConnect];
}
/* Finds the saved host role's standard socket on a background queue. A newer
 * request (after a role change) supersedes any discovery still running. */
static NSString *DefaultDiscovery(NSString **hostKey) {
    MdAccounts *accounts = NULL; NSString *path = nil;
    *hostKey = nil;
    if (!md_accounts_open_snapshot(NULL, NULL, &accounts)) {
        const MdAccount *host = md_accounts_role(accounts, MD_ACCOUNT_HOST);
        char buffer[1024];
        if (host) { *hostKey = @(host->pubkey); if (!md_management_path(host->pubkey, buffer, sizeof buffer)) path = @(buffer); }
        md_accounts_destroy(accounts);
    }
    return path;
}
- (void)locateAndConnect {
    if (_closed) return;
    NSUInteger epoch = ++_discovery;
    _locating = YES;
    if (!_snapshot) { [self showStatus:@"Looking for the host on this Mac…"]; [self render]; }
    __weak MDLocalHostPages *weakSelf = self;
    NSString *(^discover)(NSString **) = self.discover;
    if (!_discoveryQueue) _discoveryQueue = dispatch_queue_create("metadesk.manager.host-discovery", DISPATCH_QUEUE_SERIAL);
    dispatch_group_async(_qrWork, _discoveryQueue, ^{ @autoreleasepool {
        NSString *key = nil;
        NSString *path = discover ? discover(&key) : DefaultDiscovery(&key);
        dispatch_async(dispatch_get_main_queue(), ^{
            MDLocalHostPages *page = weakSelf;
            if (!page || page->_closed || epoch != page->_discovery) return; /* superseded */
            page->_locating = NO; page->_located = YES;
            /* Bound to the role: an answer that disagrees with the latest
             * role Identity reported is stale, and is never connected to. */
            NSString *expected = page->_roleKey;
            if (expected && ![(key ?: @"") isEqual:expected]) {
                page->_noHostRole = NO; page->_hostKey = nil;
                if (page->_linkPath.length && !page->_custom) [page dropLink];
                [page showStatus:@"The host role changed while looking for the host. Choose Try Again."]; [page render];
                return;
            }
            page->_noHostRole = !key;
            page->_hostKey = key ?: @"";
            if (page->_custom) { [page showStatus:nil]; [page render]; return; }
            if (path) {
                page->_endpoint.placeholderString = path;
                if (![path isEqual:page->_linkPath] || !page->_snapshot) [page connectTo:path];
            } else {
                /* No host role now: drop a link that followed the previous role. */
                if (page->_linkPath.length) [page dropLink];
                [page showStatus:nil]; [page render];
            }
        });
    } });
}
/* Invalidates everything learned from the current link before it is
 * replaced: the snapshot, pending requests, remembered clients and any
 * decision sheet. A decision made against it can no longer be sent. */
- (void)fenceLink {
    ++_linkEpoch;
    NSWindow *decision = _decision; _decision = nil;
    if (decision.sheetParent) [decision.sheetParent endSheet:decision returnCode:NSModalResponseAbort];
    _snapshot = nil; _policy = nil; _permissions = nil; _policyGeneration = nil;
    _selectedToken = nil; _selectedPeer = nil;
}
- (void)dropLink {
    [self fenceLink];
    _linkPath = nil; _busy = NO;
    [_link connect:@""];
}
/* Identity reloaded its roles. key is the host role's key ("" when none,
 * nil when unknown). The standard socket follows the role: on a change the
 * old link is fenced before the replacement is looked up. A custom socket
 * is the user's explicit choice and stays, but the standard path learned for
 * the old role is forgotten, so leaving custom mode looks it up again. */
- (void)hostRoleChanged:(NSString *)key {
    if (_closed) return;
    if (key) _roleKey = key;
    if (!_activated) return;
    if (!key) { if (!_custom) [self locateAndConnect]; return; } /* unknown: discovery decides */
    /* Unchanged (e.g. a label edit) only if no lookup is pending: a pending
     * one may have read another role in the meantime. */
    if (!_locating && _hostKey && [key isEqual:_hostKey]) return;
    _hostKey = nil; _located = NO;
    if (_custom) { [self showStatus:nil]; [self render]; return; }
    [self dropLink];
    [self showStatus:nil]; [self render];
    [self locateAndConnect];
}
- (void)reconnect:(id)sender {
    (void)sender;
    if (_custom && MDHostTrim(_endpoint.stringValue).length) [self connect:nil];
    else [self locateAndConnect];
}
- (void)buildAccessView {
    _accessStatus = [MDStatusView new];
    _notificationStatus = MDNote(@"");
    _requests = AccessTable(self, @"Pending requests");
    MDColumnNamed(_requests, @"Client", @"Client", 175, nil);
    MDColumnNamed(_requests, @"Key", @"Public Key", 160, nil);
    MDColumnNamed(_requests, @"Requested", @"Requested", 200, nil);
    MDColumnNamed(_requests, @"Left", @"Time Left", 80, nil);
    MDFinishColumns(_requests, @"Requested");
    _requests.target = self; _requests.doubleAction = @selector(review:);
    _clients = AccessTable(self, @"Remembered clients");
    MDColumnNamed(_clients, @"Client", @"Client", 175, nil);
    MDColumnNamed(_clients, @"Key", @"Public Key", 160, nil);
    MDColumnNamed(_clients, @"Access", @"Access", 260, nil);
    MDFinishColumns(_clients, @"Access");
    _clients.target = self; _clients.doubleAction = @selector(edit:);
    NSView *empty = nil; NSTextField *title = nil, *detail = nil;
    NSView *requests = MDTableArea(MDTableScroll(_requests), &empty, &title, &detail);
    _requestsEmpty = empty; _requestsEmptyTitle = title; _requestsEmptyDetail = detail;
    NSView *clients = MDTableArea(MDTableScroll(_clients), &empty, &title, &detail);
    _clientsEmpty = empty; _clientsEmptyTitle = title; _clientsEmptyDetail = detail;
    _review = MDButton(@"Review Request…", self, @selector(review:));
    NSStackView *requestRow = MDRow(@[_notificationStatus]); requestRow.alignment = NSLayoutAttributeCenterY;
    [requestRow addView:_review inGravity:NSStackViewGravityTrailing];

    _edit = MDButton(@"Edit Access…", self, @selector(edit:));
    _revoke = MDButton(@"Revoke…", self, @selector(revoke:));
    _kickRevoke = MDButton(@"Disconnect and Revoke…", self, @selector(kickRevoke:));
    NSStackView *clientRow = MDRow(@[]); clientRow.alignment = NSLayoutAttributeCenterY;
    for (NSView *v in @[_edit, _revoke, _kickRevoke]) [clientRow addView:v inGravity:NSStackViewGravityTrailing];
    NSStackView *stack = nil;
    self.accessView = MDPage(@[_accessStatus, MDHeader(@"Pending Requests"), requests, requestRow, MDSeparator(),
                               MDHeader(@"Remembered Clients"), clients, clientRow], &stack);
    [requests.heightAnchor constraintEqualToAnchor:clients.heightAnchor].active = YES;
    [stack setCustomSpacing:MDGroupSpacing afterView:_accessStatus];
    [stack setCustomSpacing:MDGroupSpacing afterView:requestRow];
    [stack setCustomSpacing:MDGroupSpacing afterView:stack.arrangedSubviews[4]];
}
- (void)enableNotifications:(UNUserNotificationCenter *)center {
    if (_closed) return;
    _notifications = center; _notifications.delegate = self;
    _notificationStatus.stringValue=@"New requests also appear as notifications."; [self updateNotices];
}
- (void)clearNotices:(NSArray *)ids {
    if (!ids.count) return;
    [_notifications removePendingNotificationRequestsWithIdentifiers:ids];
    [_notifications removeDeliveredNotificationsWithIdentifiers:ids];
}
- (void)updateNotices {
    if (!_notifications) return;
    NSSet *wanted = _closed ? [NSSet set] : MDRequestNotices(_snapshot,Now());
    NSMutableSet *removed = [_posted mutableCopy]; [removed minusSet:wanted];
    [self clearNotices:removed.allObjects]; [_posted minusSet:removed];
    for (NSString *token in _noticeAttempts.allKeys) if (![wanted containsObject:token]) {
        [_noticeAttempts removeObjectForKey:token]; [_noticeCooling removeObject:token];
    }

    __weak MDLocalHostPages *weakSelf = self;
    UNUserNotificationCenter *center = _notifications;
    __weak UNUserNotificationCenter *weakCenter = center;
    for (NSString *token in wanted) if (![_posted containsObject:token] && ![_noticeCooling containsObject:token] &&
        [_noticeAttempts[token] unsignedIntegerValue] < 2) {
        _noticeAttempts[token]=@([_noticeAttempts[token] unsignedIntegerValue]+1);
        [_posted addObject:token];
        UNMutableNotificationContent *content = [UNMutableNotificationContent new];
        content.title = @"Metadesk connection request"; content.body = @"Open Metadesk to review this request.";
        /* Intentionally no action category or approval action. */
        [center addNotificationRequest:[UNNotificationRequest requestWithIdentifier:token content:content trigger:nil]
            withCompletionHandler:^(NSError *error) { dispatch_async(dispatch_get_main_queue(), ^{
                MDLocalHostPages *page = weakSelf;
                if (!page || page->_closed || !MDRequestForToken(page->_snapshot,token,Now())) {
                    [weakCenter removePendingNotificationRequestsWithIdentifiers:@[token]];
                    [weakCenter removeDeliveredNotificationsWithIdentifiers:@[token]];
                } else if (error) [page noticeFailed:token];
            }); }];
    }
}
- (void)noticeFailed:(NSString *)token {
    [_posted removeObject:token];
    if ([_noticeAttempts[token] unsignedIntegerValue] >= 2) {
        _notificationStatus.stringValue=@"A notification couldn’t be posted. Review the request here.";
        return;
    }
    [_noticeCooling addObject:token];
    __weak MDLocalHostPages *weakSelf=self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,1000000000LL),dispatch_get_main_queue(), ^{
        MDLocalHostPages *page=weakSelf;
        if (!page || page->_closed) return;
        [page->_noticeCooling removeObject:token]; [page updateNotices];
    });
}
- (void)userNotificationCenter:(UNUserNotificationCenter *)center didReceiveNotificationResponse:(UNNotificationResponse *)response
         withCompletionHandler:(void (^)(void))completion {
    (void)center;
    NSString *token = response.notification.request.identifier;
    BOOL opened = [response.actionIdentifier isEqual:UNNotificationDefaultActionIdentifier];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self->_closed && opened && MDRequestForToken(self->_snapshot,token,Now())) {
            [NSApp sendAction:@selector(showSection:) to:nil from:@"Access"];
            [self.accessView.window makeKeyAndOrderFront:nil];
            [self openRequest:token];
        }
        completion();
    });
}
- (void)showQR:(id)sender {
    (void)sender;
    NSWindow *window = self.computerView.window;
    if (_closed || !_showQR.enabled || !window || window.attachedSheet) return;
    _showQR.enabled = NO;
    dispatch_group_async(_qrWork, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSString *fingerprint = nil;
        NSString *profile = MDSavedHostProfile(nil, &fingerprint);
        id pixels = CFBridgingRelease(MDCreateHostQR(profile));
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_closed) return;
            self->_showQR.enabled = YES;
            if (window != self.computerView.window || window.attachedSheet) return;
            MDSheet *sheet = [[MDSheet alloc] initWithTitle:@"Host QR Code"
                message:pixels ? @"Scan this from the device you’ll connect from, then check that the fingerprint it shows matches this one. This shows the saved host role; it doesn’t check that the host is running."
                               : @"There’s no saved host role to show. Assign a host role in Identity, then try again."
                  width:440];
            if (pixels) {
                CGImageRef bitmap = (__bridge CGImageRef)pixels;
                NSSize size = NSMakeSize(CGImageGetWidth(bitmap), CGImageGetHeight(bitmap));
                NSImageView *view = [NSImageView imageViewWithImage:[[NSImage alloc] initWithCGImage:bitmap size:size]];
                view.imageScaling = NSImageScaleProportionallyUpOrDown;
                view.accessibilityLabel = @"QR code for the saved public host identity";
                [view.widthAnchor constraintEqualToConstant:MIN(size.width, 280)].active = YES;
                [view.heightAnchor constraintEqualToAnchor:view.widthAnchor].active = YES;
                NSStackView *center = [NSStackView stackViewWithViews:@[view]];
                center.alignment = NSLayoutAttributeCenterX; center.orientation = NSUserInterfaceLayoutOrientationVertical;
                sheet.accessory = MDColumn(@[center, MDDetails(@[@[@"Fingerprint", MDKeyValue(fingerprint ?: @"")]])], MDGroupSpacing);
            }
            [sheet addButton:@"Done" role:MDSheetDefault response:NSAlertFirstButtonReturn];
            [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) { (void)response; }];
        });
    } });
}
- (void)close:(dispatch_block_t)drained {
    _closed = YES; [_clock invalidate]; _clock = nil;
    [self clearNotices:_posted.allObjects]; [_posted removeAllObjects];
    _notifications.delegate = nil;
    NSWindow *review = _chooser.sheet.window;
    if (review.sheetParent) [review.sheetParent endSheet:review returnCode:NSAlertFirstButtonReturn];
    dispatch_group_enter(_qrWork);
    [_link close:^{ dispatch_group_leave(self->_qrWork); }];
    dispatch_group_notify(_qrWork, dispatch_get_main_queue(), drained);
}
- (void)showStatus:(NSString *)status {
    MDStatusKind kind = _busy ? MDStatusWorking : _snapshot ? MDStatusOK : status.length ? MDStatusWarning : MDStatusInfo;
    NSString *text = status;
    if (!text.length) text = _locating && !_located ? @"Looking for the host on this Mac…" : !_located ? @"Not connected." : _noHostRole ? @"This Mac doesn’t have a host role yet. Assign one in Identity to share this Mac."
        : @"The host isn’t running, or isn’t reachable at its standard location.";
    if (_located && _noHostRole && !_snapshot && !_busy) kind = MDStatusInfo;
    [_status setText:text kind:kind];
    _retry.hidden = _busy || _snapshot != nil || !_located || (_noHostRole && !_custom);
    _openIdentity.hidden = _busy || _snapshot != nil || !_noHostRole;
    NSString *access = _snapshot || _busy ? text : @"Not connected to the host on this Mac. Requests appear here once This Computer is connected.";
    [_accessStatus setText:access kind:_snapshot || _busy ? kind : MDStatusInfo];
}
- (void)received:(NSString *)status snapshot:(NSDictionary *)s policy:(NSDictionary *)p permissions:(NSDictionary *)perms busy:(BOOL)busy {
    if (_closed) return;
    _snapshot = s; _policy = p; _permissions = perms; _busy = busy;
    /* A failed or dropped link is explained by the page's own state (no host
     * role, or not reachable); the transport detail stays in the tooltip. */
    /* After the link was dropped (no path), its own failure isn't news:
     * the page's state says what is happening (looking up, or no role). */
    [self showStatus:(s || busy || !_located) && _linkPath.length ? status : nil];
    _status.toolTip = s || busy ? nil : status;
    [self render];
    /* Read remembered clients once per host generation, so Access is never
     * waiting on a manual reload. A read, never a change. */
    if (s && !p && !busy && ![_policyGeneration isEqual:s[@"generation"]]) {
        _policyGeneration = s[@"generation"];
        [self refreshPolicy:nil];
    }
}
/* Connect with a custom socket uses it; with the field empty it leaves
 * custom mode and always looks up the standard socket afresh. */
- (void)connect:(id)sender {
    (void)sender;
    if (_closed) return;
    NSString *path = MDHostTrim(_endpoint.stringValue);
    if (path.length) { _custom = YES; [self connectTo:path]; return; }
    _custom = NO;
    [self dropLink];
    [self locateAndConnect];
}
- (void)connectTo:(NSString *)path {
    [self fenceLink];
    _linkPath = path; _busy = YES;
    [self showStatus:@"Connecting to the host on this Mac…"];
    [self render]; [_link connect:path];
}
- (BOOL)canRefreshSection:(NSString *)section {
    if (_closed || _busy) return NO;
    if ([section isEqual:@"This Computer"]) return YES; /* reconnects when not connected */
    return [section isEqual:@"Access"] && _snapshot != nil;
}
- (void)refreshSection:(NSString *)section {
    if (![self canRefreshSection:section]) return;
    if ([section isEqual:@"Access"]) [self refreshPolicy:nil];
    else if (_snapshot) [self permissions:nil];
    else [self reconnect:nil];
}
- (void)permissions:(id)sender { (void)sender; [_link command:@{@"v":@1,@"method":@"host.permissions"}]; }
- (void)refreshPolicy:(id)sender { (void)sender; [_link command:@{@"v":@1,@"method":@"access.list"}]; }
- (void)settings:(MDPermissionKind)kind {
    if (![NSWorkspace.sharedWorkspace openURL:MDPermissionSettingsURL(kind)])
        [self showStatus:@"System Settings couldn’t be opened. Open Privacy & Security in System Settings and allow the host app there."];
}
- (void)screen:(id)sender { (void)sender; [self settings:MDScreenRecording]; }
- (void)ax:(id)sender { (void)sender; [self settings:MDAccessibility]; }
- (NSDictionary *)session { id s = _snapshot[@"session"]; return [s isKindOfClass:NSDictionary.class] ? s : nil; }
- (NSDictionary *)client {
    NSInteger row = _clients.selectedRow;
    NSString *key = row >= 0 && (NSUInteger)row < _clientRows.count ? _clientRows[(NSUInteger)row][@"peer"] : nil;
    for (NSDictionary *c in _policy[@"clients"]) if ([key isEqual:c[@"peer"]]) return c;
    return nil;
}
- (NSString *)selectedRequestToken {
    NSInteger row = _requests.selectedRow;
    return row >= 0 && (NSUInteger)row < _requestRows.count ? _requestRows[(NSUInteger)row][@"token"] : nil;
}
/* Restores a selection by identity. With nothing selected before, the first
 * row is selected; a previously selected item that disappeared is never
 * replaced by a different one. */
static BOOL SelectRow(NSTableView *table, NSArray<NSDictionary *> *rows, NSString *key, id value) {
    NSUInteger index = NSNotFound;
    for (NSUInteger i = 0; value && i < rows.count; ++i) if ([rows[i][key] isEqual:value]) { index = i; break; }
    if (!value && rows.count) index = 0;
    if (index == NSNotFound) [table deselectAll:nil];
    else [table selectRowIndexes:[NSIndexSet indexSetWithIndex:index] byExtendingSelection:NO];
    return index != NSNotFound;
}
static void Permission(MDStatusView *view, NSDictionary *permissions, NSString *key, BOOL connected) {
    if (!permissions) [view setText:connected ? @"Unknown — choose Recheck Permissions" : @"Unknown — connect to check" kind:MDStatusInfo];
    else if ([permissions[key] boolValue]) [view setText:@"Allowed" kind:MDStatusOK];
    else [view setText:@"Not allowed" kind:MDStatusWarning];
}
- (void)render {
    NSDictionary *h = _snapshot[@"host"], *s = self.session;
    NSString *state = MDHostBoundText(h[@"state"], 32);
    _agent.stringValue = h ? [NSString stringWithFormat:@"Running %@%@.", [h[@"managed"] boolValue] ? @"in the background" : @"in a Terminal session",
                                 [state isEqual:@"waiting"] ? @", waiting for a client" : @""] :
        _snapshot ? @"Running." : @"Unknown until connected.";
    _agent.toolTip = h ? [NSString stringWithFormat:@"Process %@, state “%@”", h[@"pid"], state] : nil;
    _agent.textColor = h || _snapshot ? NSColor.labelColor : NSColor.secondaryLabelColor;
    _session.stringValue = !s ? (_snapshot ? @"No one is connected." : @"Unknown until connected.") :
        [s[@"revoked"] boolValue] ? @"A client is being disconnected." :
        [NSString stringWithFormat:@"A client is connected or about to connect, allowed to: %@.", MDAccessCaps([s[@"granted"] unsignedIntValue]).lowercaseString];
    _sessionKey.stringValue = s ? MDHostFingerprint(s[@"peer"]) ?: @"Unknown client" : @"";
    _sessionKey.toolTip = s ? MDHostNpub(s[@"peer"]) : nil;
    MDSetRowHidden(_sessionKey, !s);
    _session.textColor = s ? NSColor.labelColor : NSColor.secondaryLabelColor;
    Permission(_screen, _permissions, @"screen_recording", _snapshot != nil);
    Permission(_ax, _permissions, @"accessibility", _snapshot != nil);
    _connect.enabled = !_busy; _endpoint.enabled = !_busy;
    _agent.textColor = _session.textColor = _snapshot ? NSColor.labelColor : NSColor.secondaryLabelColor;
    _recheck.enabled = _snapshot && !_busy; _kick.enabled = !_busy && MDKickCommand(_snapshot,s) != nil;
    MDSetRowHidden(_kick, !s);

    NSDictionary *selected = self.client;
    NSString *peer = selected[@"peer"] ?: _selectedPeer;
    NSMutableArray *clients = [NSMutableArray array];
    if ([_policy[@"policy_available"] boolValue]) for (NSDictionary *c in _policy[@"clients"]) {
        uint32_t bits;
        BOOL known = ![c[@"review_required"] boolValue] && MDPolicyCaps(c[@"caps"], &bits);
        [clients addObject:@{@"peer":c[@"peer"] ?: @"", @"fingerprint":MDHostFingerprint(c[@"peer"]) ?: @"Invalid key",
            @"npub":MDHostNpub(c[@"peer"]) ?: @"Invalid key", @"access":known ? MDAccessCaps(bits) : @"Unknown or legacy — review with the CLI"}];
    }
    _clientRows = clients;
    [_clients reloadData];
    SelectRow(_clients, _clientRows, @"peer", peer);
    BOOL available = [_policy[@"policy_available"] boolValue];
    _clientsEmpty.hidden = _clientRows.count != 0;
    _clientsEmptyTitle.stringValue = !_snapshot ? @"Not Connected" : available ? @"No Remembered Clients" : @"Remembered Clients Not Loaded";
    _clientsEmptyDetail.stringValue = !_snapshot ? @"Connect to the local host in This Computer." :
        available ? @"Clients you approve with Approve and Remember appear here." : @"Loading remembered clients…";
    [self tick:nil];
}
- (void)tick:(NSTimer *)timer {
    (void)timer;
    if (_closed) return;
    NSTimeInterval now = Now();
    NSArray *requests = MDLiveRequests(_snapshot, now);
    NSString *selected = self.selectedRequestToken ?: _selectedToken;
    NSMutableArray *rows = [NSMutableArray array];
    for (NSDictionary *r in requests) {
        NSString *token = MDRequestToken(_snapshot,r);
        long long left = (long long)ceil([r[@"expires_at"] doubleValue] - now);
        [rows addObject:@{@"token":token ?: @"", @"fingerprint":MDHostFingerprint(r[@"peer"]) ?: @"Invalid key",
            @"npub":MDHostNpub(r[@"peer"]) ?: @"Invalid key", @"requested":MDAccessCaps([r[@"requested"] unsignedIntValue]),
            @"left":left >= 60 ? [NSString stringWithFormat:@"%lld min %02lld s", left / 60, left % 60] : [NSString stringWithFormat:@"%lld s", left]}];
    }
    _requestRows = rows;
    [_requests reloadData];
    SelectRow(_requests, _requestRows, @"token", selected);
    _requestsEmpty.hidden = rows.count != 0;
    _requestsEmptyTitle.stringValue = _snapshot ? @"No Pending Requests" : @"Not Connected";
    _requestsEmptyDetail.stringValue = _snapshot ? @"When a client asks to connect, its request waits here for your review." : @"Connect to the local host in This Computer.";
    if (requests.count != _pendingCount) { _pendingCount = requests.count; if (self.pendingChanged) self.pendingChanged(_pendingCount); }
    [self selection:nil];
    [self updateNotices];
    if (_chooser.update) _chooser.update();
    if ((requests.count || _chooser) && !_clock) {
        _clock = [NSTimer timerWithTimeInterval:1 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
        [NSRunLoop.mainRunLoop addTimer:_clock forMode:NSRunLoopCommonModes];
    } else if (!requests.count && !_chooser) { [_clock invalidate]; _clock=nil; }
}
- (void)selection:(id)sender {
    (void)sender;
    NSString *token = self.selectedRequestToken;
    if (token) _selectedToken = token;
    NSDictionary *r = MDRequestForToken(_snapshot,token,Now());
    _review.enabled = r && !_busy;
    NSDictionary *c = self.client;
    if (c) _selectedPeer = c[@"peer"];
    uint32_t bits;
    BOOL editable = c && ![c[@"review_required"] boolValue] && MDPolicyCaps(c[@"caps"], &bits);
    _edit.enabled = editable && !_busy; _revoke.enabled = c && !_busy;
    _kickRevoke.enabled = c && !_busy && [self.session[@"peer"] isEqual:c[@"peer"]];
}
- (void)tableViewSelectionDidChange:(NSNotification *)note { (void)note; [self selection:nil]; }
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table {
    return (NSInteger)(table == _requests ? _requestRows.count : _clientRows.count);
}
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    NSDictionary *r = (table == _requests ? _requestRows : _clientRows)[(NSUInteger)row];
    NSString *identifier = column.identifier;
    NSString *text = [identifier isEqual:@"Client"] ? r[@"fingerprint"] : [identifier isEqual:@"Key"] ? r[@"npub"] :
        [identifier isEqual:@"Requested"] ? r[@"requested"] : [identifier isEqual:@"Left"] ? r[@"left"] : r[@"access"];
    NSTableCellView *cell = MDCell(table, [identifier stringByAppendingString:@"Cell"], text);
    BOOL key = [identifier isEqual:@"Client"] || [identifier isEqual:@"Key"];
    cell.textField.font = key ? [NSFont monospacedSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular] : [NSFont systemFontOfSize:NSFont.systemFontSize];
    cell.textField.lineBreakMode = [identifier isEqual:@"Key"] ? NSLineBreakByTruncatingMiddle : NSLineBreakByTruncatingTail;
    cell.textField.toolTip = [identifier isEqual:@"Client"] ? r[@"npub"] : text;
    return cell;
}
- (NSString *)tableView:(NSTableView *)table typeSelectStringForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)column;
    return (table == _requests ? _requestRows : _clientRows)[(NSUInteger)row][@"fingerprint"];
}
/* An approval is a trust decision: no button answers Return, Escape cancels,
 * and nothing can be approved once the request has expired, been cancelled or
 * gone stale. Only requested and supported screen viewing starts selected. */
/* A notification names one request: review exactly that one, or nothing. */
- (void)openRequest:(NSString *)token {
    if (_closed || !token || !MDRequestForToken(_snapshot, token, Now())) return;
    [self tick:nil];
    if (!SelectRow(_requests, _requestRows, @"token", token) || ![self.selectedRequestToken isEqual:token]) return;
    _selectedToken = token;
    [self reviewToken:token];
}
- (void)review:(id)sender { (void)sender; [self reviewToken:self.selectedRequestToken]; }
- (void)reviewToken:(NSString *)token {
    NSWindow *window = self.accessView.window;
    NSDictionary *snapshot = _snapshot, *policy = _policy;
    NSDictionary *r = MDRequestForToken(snapshot,token,Now());
    if (!r || _busy || _closed || _chooser || !window || window.attachedSheet) return;
    MDSheet *sheet = [[MDSheet alloc] initWithTitle:@"Allow This Client to Connect?"
        message:@"Compare the fingerprint with the one the person connecting sees. Only the permissions you check are granted."
          width:540];
    MDCapChooser *chooser = [MDCapChooser new]; chooser.sheet = sheet;
    NSView *boxes = [chooser viewWithAllowed:MDApprovalAllowed(snapshot,r) initial:MDApprovalDefault(snapshot,r)];
    NSTextField *fingerprint = MDKeyValue(MDHostFingerprint(r[@"peer"]) ?: @"Invalid key");
    fingerprint.font = [NSFont monospacedSystemFontOfSize:NSFont.systemFontSize + 2 weight:NSFontWeightMedium];
    fingerprint.toolTip = [@"Request " stringByAppendingString:MDHostBoundText(r[@"request_id"], 64)];
    NSGridView *details = MDDetails(@[@[@"Fingerprint", fingerprint],
        @[@"Public key", MDKeyValue(MDHostNpub(r[@"peer"]) ?: @"Invalid key")],
        @[@"Requests", MDAccessCaps([r[@"requested"] unsignedIntValue])]]);
    chooser.lifetime = [MDStatusView new];
    NSGridView *grant = MDForm(@[@[@"Allow", boxes],
        @[NSNull.null, MDNote(@"Permissions the client didn’t request, or that this host doesn’t support, can’t be checked.")],
        @[NSNull.null, chooser.lifetime]]);
    MDAlignForms(@[details, grant]);
    sheet.accessory = MDColumn(@[details, grant], MDGroupSpacing);
    [sheet addButton:@"Deny" role:MDSheetOther response:NSAlertThirdButtonReturn+1];
    [sheet addButton:@"Cancel" role:MDSheetCancel response:NSAlertFirstButtonReturn];
    NSButton *remember = [sheet addButton:@"Approve and Remember" role:MDSheetAction response:NSAlertThirdButtonReturn];
    remember.toolTip = @"Approve now and remember exactly these permissions for this client.";
    [sheet addButton:@"Approve Once" role:MDSheetAction response:NSAlertSecondButtonReturn];
    _chooser = chooser;
    __weak MDLocalHostPages *weakSelf = self;
    __weak MDCapChooser *weakChooser = chooser;
    chooser.update = ^{
        MDLocalHostPages *page = weakSelf; MDCapChooser *form = weakChooser;
        if (!page || !form) return;
        BOOL current = !page->_busy && [snapshot[@"revision"] isEqual:page->_snapshot[@"revision"]] &&
            MDRequestForToken(page->_snapshot,token,Now()) != nil;
        long long remaining = (long long)ceil([r[@"expires_at"] doubleValue] - Now());
        if (current) [form.lifetime setText:[NSString stringWithFormat:@"This request expires in %@.",
            remaining >= 60 ? [NSString stringWithFormat:@"%lld min %02lld s", remaining / 60, remaining % 60] : [NSString stringWithFormat:@"%lld s", remaining]] kind:MDStatusInfo];
        else [form.lifetime setText:@"This request expired, was cancelled or changed, so it can’t be approved. Choose Cancel." kind:MDStatusError];
        [form.sheet buttonWithResponse:NSAlertSecondButtonReturn].enabled = current && form.selected;
        [form.sheet buttonWithResponse:NSAlertThirdButtonReturn].enabled = current && form.selected && [policy[@"policy_available"] boolValue] &&
            [page->_policy[@"policy_version"] isEqual:policy[@"policy_version"]];
        [form.sheet buttonWithResponse:NSAlertThirdButtonReturn+1].enabled = current;
    };
    chooser.update();
    NSUInteger epoch = _linkEpoch;
    _decision = sheet.window;
    [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        self->_chooser = nil; if (self->_decision == sheet.window) self->_decision = nil;
        [self tick:nil];
        if (self->_closed || self->_busy || epoch != self->_linkEpoch || response < NSAlertSecondButtonReturn || response > NSAlertThirdButtonReturn+1 ||
            ![snapshot[@"revision"] isEqual:self->_snapshot[@"revision"]]) return;
        BOOL deny = response == NSAlertThirdButtonReturn+1, remember = response == NSAlertThirdButtonReturn;
        if (remember && (![self->_policy[@"policy_available"] boolValue] || ![policy[@"policy_version"] isEqual:self->_policy[@"policy_version"]])) return;
        NSTimeInterval now = Now();
        NSDictionary *current = MDRequestForToken(self->_snapshot,token,now);
        NSDictionary *cmd = MDApprovalCommand(self->_snapshot,current,deny ? @"deny" : @"approve",chooser.selected,remember,now);
        if (cmd) [self->_link command:cmd policyVersion:remember ? policy[@"policy_version"] : nil];
    }];
}
- (void)confirm:(NSString *)title info:(NSString *)info button:(NSString *)button window:(NSWindow *)window action:(dispatch_block_t)action {
    if (_closed || _busy || !window || window.attachedSheet) return;
    NSAlert *a = MDAlert(title, info);
    MDAlertDestructive(a, button);
    NSUInteger epoch = _linkEpoch;
    _decision = a.window;
    [a beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (self->_decision == a.window) self->_decision = nil;
        if (!self->_closed && epoch == self->_linkEpoch && response == NSAlertSecondButtonReturn) action();
    }];
}
- (void)kick:(id)sender {
    (void)sender;
    NSDictionary *cmd = MDKickCommand(_snapshot,self.session);
    if (cmd) [self confirm:@"Disconnect This Session?"
                      info:@"The client is disconnected now. Its remembered access doesn’t change, so it can ask to connect again."
                    button:@"Disconnect" window:self.computerView.window action:^{ [self->_link command:cmd]; }];
}
- (void)remove:(BOOL)kick {
    NSDictionary *c = self.client;
    NSDictionary *cmd = MDPolicyCommand(_snapshot,_policy,c,0,YES,kick ? self.session[@"session_id"] : nil);
    NSString *fingerprint = MDHostFingerprint(c[@"peer"]) ?: @"this client";
    NSString *info = [NSString stringWithFormat:@"The client with fingerprint %@ loses its remembered access%@ and will need a new approval to connect. "
        @"If the host doesn’t confirm, reload the list — a failed reply doesn’t prove the access is gone.",
        fingerprint, kick ? @", its current session ends," : @""];
    if (cmd) [self confirm:kick ? @"Disconnect and Revoke Access?" : @"Revoke Access?" info:info
                    button:kick ? @"Disconnect and Revoke" : @"Revoke" window:self.accessView.window action:^{ [self->_link command:cmd]; }];
}
- (void)revoke:(id)sender { (void)sender; [self remove:NO]; }
- (void)kickRevoke:(id)sender { (void)sender; if (self.session) [self remove:YES]; }
- (void)edit:(id)sender {
    (void)sender;
    NSDictionary *s = _snapshot, *p = _policy, *c = self.client;
    uint32_t mask;
    NSWindow *window = self.accessView.window;
    if (_closed || _busy || !window || window.attachedSheet || !MDPolicyCaps(c[@"caps"], &mask) || [c[@"review_required"] boolValue]) return;
    MDSheet *sheet = [[MDSheet alloc] initWithTitle:@"Edit Remembered Access"
        message:@"The client’s remembered access is replaced with exactly the permissions you check. It’s disconnected and must connect again."
          width:520];
    MDCapChooser *chooser=[MDCapChooser new]; chooser.sheet = sheet;
    NSView *boxes = [chooser viewWithAllowed:MD_HOST_ACCESS_CAPS initial:mask];
    NSGridView *form = MDForm(@[@[@"Client", MDKeyValue(MDHostFingerprint(c[@"peer"]) ?: @"Invalid key")],
                                @[@"Public key", MDKeyValue(MDHostNpub(c[@"peer"]) ?: @"Invalid key")], @[@"Allow", boxes]]);
    sheet.accessory = form;
    [sheet addButton:@"Cancel" role:MDSheetCancel response:NSAlertFirstButtonReturn];
    [sheet addButton:@"Save Access" role:MDSheetDefault response:NSAlertSecondButtonReturn];
    NSUInteger epoch = _linkEpoch;
    _decision = sheet.window;
    [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (self->_decision == sheet.window) self->_decision = nil;
        if (!self->_closed && epoch == self->_linkEpoch && response == NSAlertSecondButtonReturn) [self->_link command:MDPolicyCommand(s,p,c,chooser.selected,NO,nil)];
    }];
}

@end