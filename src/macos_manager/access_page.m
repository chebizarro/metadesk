#import "access_page.h"
#import "access_model.h"
#import "hosts_model.h"
#import "host_qr.h"
#import "model.h"
#import <Security/Security.h>
#import <UserNotifications/UserNotifications.h>

static NSTimeInterval Now(void) { return NSDate.date.timeIntervalSince1970; }
static NSTextField *Label(NSString *s) { return [NSTextField wrappingLabelWithString:s ?: @""]; }
static NSButton *Button(id self, NSString *s, SEL sel) { return [NSButton buttonWithTitle:s target:self action:sel]; }
static NSStackView *Stack(NSArray *views, BOOL horizontal) {
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = horizontal ? NSUserInterfaceLayoutOrientationHorizontal : NSUserInterfaceLayoutOrientationVertical;
    s.alignment = horizontal ? NSLayoutAttributeCenterY : NSLayoutAttributeLeading; s.spacing = 8;
    return s;
}
static NSView *Column(NSArray *views) {
    NSView *root = [NSView new]; NSStackView *s = Stack(@[], NO); s.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSView *v in views) { [s addArrangedSubview:v]; [v.widthAnchor constraintLessThanOrEqualToAnchor:s.widthAnchor].active = YES; }
    [root addSubview:s];
    [NSLayoutConstraint activateConstraints:@[[s.topAnchor constraintEqualToAnchor:root.topAnchor constant:14],
        [s.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:14], [s.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-14]]];
    return root;
}
static NSPopUpButton *Picker(id self, SEL action) {
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO]; p.target = self; p.action = action; return p;
}
static void Item(NSPopUpButton *p, NSString *label, id key) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:label action:NULL keyEquivalent:@""];
    item.representedObject = key; [p.menu addItem:item];
}
static void Select(NSPopUpButton *p, id key) {
    for (NSMenuItem *item in p.itemArray) if ([key isEqual:item.representedObject]) { [p selectItem:item]; return; }
    [p selectItemAtIndex:p.numberOfItems ? 0 : -1];
}
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

@interface MDCapChooser : NSObject
@property(nonatomic) NSArray<NSButton *> *choices;
@property(nonatomic) NSTextField *lifetime;
@property(nonatomic) NSAlert *alert;
@property(nonatomic, copy) dispatch_block_t update;
- (NSView *)viewWithAllowed:(uint32_t)allowed initial:(uint32_t)initial lifetime:(BOOL)lifetime;
- (uint32_t)selected;
@end
@implementation MDCapChooser
static const uint32_t Caps[] = {MD_CAP_VIDEO, MD_CAP_AGENT, MD_CAP_INPUT, MD_CAP_MONITOR, MD_CAP_CLIPBOARD};
- (NSView *)viewWithAllowed:(uint32_t)allowed initial:(uint32_t)initial lifetime:(BOOL)lifetime {
    NSMutableArray *list = [NSMutableArray array];
    for (NSUInteger i=0;i<sizeof(Caps)/sizeof(*Caps);++i) {
        NSButton *b = [NSButton checkboxWithTitle:MDAccessCaps(Caps[i]) target:self action:@selector(changed:)];
        b.enabled = (allowed & Caps[i]) != 0; b.state = (initial & Caps[i]) ? NSControlStateValueOn : NSControlStateValueOff;
        [list addObject:b];
    }
    self.choices = [list copy];
    if (lifetime) { self.lifetime = Label(@""); [list addObject:self.lifetime]; }
    NSStackView *s = Stack(list, NO); s.frame = NSMakeRect(0, 0, 380, lifetime ? 166 : 138); return s;
}
- (void)changed:(id)sender { (void)sender; if (self.update) self.update(); }
- (uint32_t)selected {
    uint32_t mask = 0;
    for (NSUInteger i=0;i<self.choices.count;++i) if (self.choices[i].state == NSControlStateValueOn) mask |= Caps[i];
    return mask;
}
@end

@interface MDLocalHostPages () <UNUserNotificationCenterDelegate> {
    MDHostLink *_link;
    dispatch_group_t _qrWork;
    NSButton *_showQR;
    NSDictionary *_snapshot, *_policy, *_permissions;
    BOOL _busy, _closed;
    NSTextField *_endpoint, *_status, *_agent, *_session, *_permission, *_accessStatus, *_requestDetails, *_policyDetail, *_notificationStatus;
    NSPopUpButton *_requests, *_clients;
    NSButton *_connect, *_recheck, *_kick, *_review, *_refreshPolicy, *_edit, *_revoke, *_kickRevoke;
    NSTimer *_clock;
    MDCapChooser *_chooser;
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
    _endpoint = [NSTextField textFieldWithString:@""];
    _endpoint.placeholderString = @"Absolute path to the host management socket";
    _endpoint.accessibilityLabel = @"Host management socket";
    [_endpoint.widthAnchor constraintGreaterThanOrEqualToConstant:430].active = YES;
    _status = Label(@"No authenticated host connection."); _agent = Label(@""); _session = Label(@""); _permission = Label(@"");
    _connect = Button(self, @"Connect to Local Host", @selector(connect:));
    _kick = Button(self, @"Disconnect Session…", @selector(kick:));
    _recheck = Button(self, @"Recheck Permissions", @selector(permissions:));
    _qrWork = dispatch_group_create();
    _showQR = Button(self, @"Show Host QR Code…", @selector(showQR:));
    self.computerView = Column(@[_showQR, _endpoint, _connect, _status, _agent,
        Label(@"Agent start/stop is not available here. The installed app must register dev.metadesk.host.plist before login can start the opted-in agent. For manual operation, provision a host role and start metadesk-host with --managed and --management-socket PATH; stop that foreground process with Control-C. See Host Lifecycle."),
        Label(@"Sharing and unattended access are not configurable through current host IPC."),
        _session, _kick, _permission,
        Label(@"Grant permissions to the responsible host identity, not this manager. The preflight does not request consent, prove capture or input readiness, or grant remote access."), Stack(@[_recheck, Button(self,@"Screen Recording Settings",@selector(screen:)),
        Button(self,@"Accessibility Settings",@selector(ax:))], YES)]);
    _requests = Picker(self,@selector(selection:)); _clients = Picker(self,@selector(selection:));
    _accessStatus = Label(@""); _requestDetails = Label(@""); _policyDetail = Label(@"");
    _review = Button(self,@"Review Request…",@selector(review:));
    _refreshPolicy = Button(self,@"Refresh Private Policy",@selector(refreshPolicy:));
    _edit = Button(self,@"Edit Capabilities…",@selector(edit:));
    _revoke = Button(self,@"Revoke…",@selector(revoke:));
    _kickRevoke = Button(self,@"Kick and Revoke…",@selector(kickRevoke:));
    _notificationStatus=Label(@"System notifications require an already-authorized signed app. The inbox works without them.");
    self.accessView = Column(@[_accessStatus, _notificationStatus, Label(@"Pending Requests"), _requests, _requestDetails, _review,
        Label(@"Remembered Clients"), _clients, _policyDetail, _refreshPolicy, Stack(@[_edit,_revoke,_kickRevoke], YES)]);
    _posted = [NSMutableSet set]; _noticeCooling=[NSMutableSet set]; _noticeAttempts=[NSMutableDictionary dictionary];
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
- (void)enableNotifications:(UNUserNotificationCenter *)center {
    if (_closed) return;
    _notifications = center; _notifications.delegate = self;
    _notificationStatus.stringValue=@""; [self updateNotices];
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
    if (!wanted.count) _notificationStatus.stringValue=@"";
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
        _notificationStatus.stringValue=@"A notification could not be posted. Review the request in this inbox.";
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
            NSWindow *window = self.accessView.window ?: self.computerView.window;
            if ([window.contentView isKindOfClass:NSTabView.class]) [(NSTabView *)window.contentView selectTabViewItemWithIdentifier:@"Access"];
            [window makeKeyAndOrderFront:nil];
            Select(self->_requests, token);
            [self review:nil];
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
            NSAlert *alert = [NSAlert new];
            alert.messageText = @"Host QR Code";
            [alert addButtonWithTitle:@"Close"];
            if (pixels) {
                alert.informativeText = [NSString stringWithFormat:@"Saved public host identity (not a live host check).\nFingerprint: %@", fingerprint];
                CGImageRef bitmap = (__bridge CGImageRef)pixels;
                NSSize size = NSMakeSize(CGImageGetWidth(bitmap), CGImageGetHeight(bitmap));
                NSImageView *view = [NSImageView imageViewWithImage:[[NSImage alloc] initWithCGImage:bitmap size:size]];
                view.frame = (NSRect){NSZeroPoint, size};
                view.accessibilityLabel = @"QR code for the saved public host identity";
                alert.accessoryView = view;
            } else alert.informativeText = @"Cannot display a QR code. Check the saved host role in Identity.";
            [alert beginSheetModalForWindow:window completionHandler:nil];
        });
    } });
}
- (void)close:(dispatch_block_t)drained {
    _closed = YES; [_clock invalidate]; _clock = nil;
    [self clearNotices:_posted.allObjects]; [_posted removeAllObjects];
    _notifications.delegate = nil;
    if (_chooser.alert.window.sheetParent) [_chooser.alert.window.sheetParent endSheet:_chooser.alert.window returnCode:NSAlertFirstButtonReturn];
    dispatch_group_enter(_qrWork);
    [_link close:^{ dispatch_group_leave(self->_qrWork); }];
    dispatch_group_notify(_qrWork, dispatch_get_main_queue(), drained);
}
- (void)received:(NSString *)status snapshot:(NSDictionary *)s policy:(NSDictionary *)p permissions:(NSDictionary *)perms busy:(BOOL)busy {
    if (_closed) return;
    _snapshot = s; _policy = p; _permissions = perms; _busy = busy;
    _status.stringValue = status ?: @""; _accessStatus.stringValue = status ?: @"";
    [self render];
}
- (void)connect:(id)sender {
    (void)sender;
    if (_closed) return;
    _snapshot = nil; _policy = nil; _permissions = nil; _busy = YES;
    _status.stringValue = _accessStatus.stringValue = @"Connecting to the local host…";
    [self render]; [_link connect:MDHostTrim(_endpoint.stringValue)];
}
- (void)permissions:(id)sender { (void)sender; [_link command:@{@"v":@1,@"method":@"host.permissions"}]; }
- (void)refreshPolicy:(id)sender { (void)sender; [_link command:@{@"v":@1,@"method":@"access.list"}]; }
- (void)settings:(MDPermissionKind)kind {
    if (![NSWorkspace.sharedWorkspace openURL:MDPermissionSettingsURL(kind)])
        _permission.stringValue = @"Open System Settings → Privacy & Security and allow the intended host identity.";
}
- (void)screen:(id)sender { (void)sender; [self settings:MDScreenRecording]; }
- (void)ax:(id)sender { (void)sender; [self settings:MDAccessibility]; }
- (NSDictionary *)session { id s = _snapshot[@"session"]; return [s isKindOfClass:NSDictionary.class] ? s : nil; }
- (NSDictionary *)client {
    NSString *key = _clients.selectedItem.representedObject;
    for (NSDictionary *c in _policy[@"clients"]) if ([key isEqual:c[@"peer"]]) return c;
    return nil;
}
- (void)render {
    NSDictionary *h = _snapshot[@"host"], *s = self.session;
    _agent.stringValue = h ? [NSString stringWithFormat:@"%@ host is running and reachable (PID %@, %@).", [h[@"managed"] boolValue] ? @"Managed" : @"Interactive", h[@"pid"], MDHostBoundText(h[@"state"],32)] :
        _snapshot ? @"Host reachable; supervisor state not exposed." : @"Host reachability and agent state are unknown.";
    _session.stringValue = !s ? (_snapshot ? @"No session reserved or active." : @"Session state unknown.") :
        [NSString stringWithFormat:@"%@\n%@ · %@", [s[@"revoked"] boolValue] ? @"Session ending" : @"Session reserved or active", MDHostNpub(s[@"peer"]), MDAccessCaps([s[@"granted"] unsignedIntValue])];
    _permission.stringValue = [NSString stringWithFormat:@"Host permissions: Screen Recording — %@; Accessibility — %@.",
        !_permissions ? @"Unknown" : [_permissions[@"screen_recording"] boolValue] ? @"Allowed" : @"Not allowed",
        !_permissions ? @"Unknown" : [_permissions[@"accessibility"] boolValue] ? @"Allowed" : @"Not allowed"];
    _connect.enabled = !_busy; _recheck.enabled = _snapshot && !_busy; _kick.enabled = !_busy && MDKickCommand(_snapshot,s) != nil;
    _refreshPolicy.enabled = _snapshot && !_busy;
    id selected = _clients.selectedItem.representedObject;
    [_clients removeAllItems];
    if ([_policy[@"policy_available"] boolValue]) for (NSDictionary *c in _policy[@"clients"]) Item(_clients, MDHostNpub(c[@"peer"]) ?: @"Invalid key", c[@"peer"]);
    if (!_clients.numberOfItems) Item(_clients, [_policy[@"policy_available"] boolValue] ? @"No remembered clients" : @"Private policy unavailable", nil);
    Select(_clients, selected);
    [self tick:nil];
}
- (void)tick:(NSTimer *)timer {
    (void)timer;
    if (_closed) return;
    NSArray *requests = MDLiveRequests(_snapshot, Now());
    NSString *selected = _requests.selectedItem.representedObject;
    [_requests removeAllItems];
    for (NSDictionary *r in requests) Item(_requests, MDHostNpub(r[@"peer"]) ?: @"Invalid key", MDRequestToken(_snapshot,r));
    if (!requests.count) Item(_requests, @"No pending requests", nil);
    Select(_requests, selected);
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
    NSDictionary *r = MDRequestForToken(_snapshot,_requests.selectedItem.representedObject,Now());
    _requestDetails.stringValue = r ? [NSString stringWithFormat:@"Requested: %@ · %lld s remaining", MDAccessCaps([r[@"requested"] unsignedIntValue]), (long long)ceil([r[@"expires_at"] doubleValue]-Now())] : @"";
    _review.enabled = r && !_busy;
    NSDictionary *c = self.client;
    uint32_t bits;
    BOOL editable = c && ![c[@"review_required"] boolValue] && MDPolicyCaps(c[@"caps"], &bits);
    _policyDetail.stringValue = c ? [NSString stringWithFormat:@"%@\nFingerprint: %@\n%@", MDHostNpub(c[@"peer"]), MDHostFingerprint(c[@"peer"]),
        editable ? MDAccessCaps(bits) : @"Unknown or legacy policy — review with the CLI; editing is unavailable."] : @"";
    _edit.enabled = editable && !_busy; _revoke.enabled = c && !_busy;
    _kickRevoke.enabled = c && !_busy && [self.session[@"peer"] isEqual:c[@"peer"]];
}
- (void)review:(id)sender {
    (void)sender;
    NSWindow *window = self.accessView.window;
    NSDictionary *snapshot = _snapshot, *policy = _policy;
    NSString *token = _requests.selectedItem.representedObject;
    NSDictionary *r = MDRequestForToken(snapshot,token,Now());
    if (!r || _busy || _closed || _chooser || !window || window.attachedSheet) return;
    NSAlert *a = [NSAlert new]; a.messageText = @"Review Connection Request";
    a.informativeText = [NSString stringWithFormat:@"Client: %@\nFingerprint: %@\nRequest ID: %@\nRequested: %@\nChoose only the permissions you intend to grant.",
        MDHostNpub(r[@"peer"]), MDHostFingerprint(r[@"peer"]), r[@"request_id"], MDAccessCaps([r[@"requested"] unsignedIntValue])];
    [a addButtonWithTitle:@"Cancel"]; [a addButtonWithTitle:@"Approve Once"]; [a addButtonWithTitle:@"Remember Exact Grant"]; [a addButtonWithTitle:@"Deny"];
    MDCapChooser *chooser = [MDCapChooser new]; chooser.alert = a;
    a.accessoryView = [chooser viewWithAllowed:MDApprovalAllowed(snapshot,r) initial:MDApprovalDefault(snapshot,r) lifetime:YES];
    _chooser = chooser;
    __weak MDLocalHostPages *weakSelf = self;
    __weak MDCapChooser *weakChooser = chooser;
    chooser.update = ^{
        MDLocalHostPages *page = weakSelf; MDCapChooser *form = weakChooser;
        if (!page || !form) return;
        BOOL current = !page->_busy && [snapshot[@"revision"] isEqual:page->_snapshot[@"revision"]] &&
            MDRequestForToken(page->_snapshot,token,Now()) != nil;
        NSTimeInterval remaining = [r[@"expires_at"] doubleValue] - Now();
        form.lifetime.stringValue = current ? [NSString stringWithFormat:@"%lld s remaining", (long long)ceil(remaining)] : @"Request expired, cancelled, or changed. Close this review.";
        form.alert.buttons[1].enabled = current && form.selected;
        form.alert.buttons[2].enabled = current && form.selected && [policy[@"policy_available"] boolValue] &&
            [page->_policy[@"policy_version"] isEqual:policy[@"policy_version"]];
        form.alert.buttons[3].enabled = current;
    };
    chooser.update();
    [a beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        self->_chooser = nil; [self tick:nil];
        if (self->_closed || self->_busy || response < NSAlertSecondButtonReturn || response > NSAlertThirdButtonReturn+1 ||
            ![snapshot[@"revision"] isEqual:self->_snapshot[@"revision"]]) return;
        BOOL deny = response == NSAlertThirdButtonReturn+1, remember = response == NSAlertThirdButtonReturn;
        if (remember && (![self->_policy[@"policy_available"] boolValue] || ![policy[@"policy_version"] isEqual:self->_policy[@"policy_version"]])) return;
        NSTimeInterval now = Now();
        NSDictionary *current = MDRequestForToken(self->_snapshot,token,now);
        NSDictionary *cmd = MDApprovalCommand(self->_snapshot,current,deny ? @"deny" : @"approve",chooser.selected,remember,now);
        if (cmd) [self->_link command:cmd policyVersion:remember ? policy[@"policy_version"] : nil];
    }];
}
- (void)confirm:(NSString *)title text:(NSString *)text window:(NSWindow *)window action:(dispatch_block_t)action {
    if (_closed || _busy || !window || window.attachedSheet) return;
    NSAlert *a=[NSAlert new]; a.messageText=title; a.informativeText=text;
    [a addButtonWithTitle:@"Cancel"]; [a addButtonWithTitle:title];
    [a beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (!self->_closed && response == NSAlertSecondButtonReturn) action();
    }];
}
- (void)kick:(id)sender {
    (void)sender;
    NSDictionary *cmd = MDKickCommand(_snapshot,self.session);
    if (cmd) [self confirm:@"Disconnect Session" text:@"End this session without changing remembered access?" window:self.computerView.window action:^{ [self->_link command:cmd]; }];
}
- (void)remove:(BOOL)kick {
    NSDictionary *c = self.client;
    NSDictionary *cmd = MDPolicyCommand(_snapshot,_policy,c,0,YES,kick ? self.session[@"session_id"] : nil);
    if (cmd) [self confirm:kick ? @"Kick and Revoke" : @"Revoke Access" text:@"Remove this private grant and end any admission authority for this client?" window:self.accessView.window action:^{ [self->_link command:cmd]; }];
}
- (void)revoke:(id)sender { (void)sender; [self remove:NO]; }
- (void)kickRevoke:(id)sender { (void)sender; if (self.session) [self remove:YES]; }
- (void)edit:(id)sender {
    (void)sender;
    NSDictionary *s = _snapshot, *p = _policy, *c = self.client;
    uint32_t mask;
    NSWindow *window = self.accessView.window;
    if (_closed || _busy || !window || window.attachedSheet || !MDPolicyCaps(c[@"caps"], &mask) || [c[@"review_required"] boolValue]) return;
    NSAlert *a = [NSAlert new]; a.messageText = @"Edit Remembered Capabilities";
    a.informativeText = [NSString stringWithFormat:@"%@\nReplace the grant with exactly the selected capabilities. This ends existing admission for this client.",MDHostNpub(c[@"peer"])];
    MDCapChooser *chooser=[MDCapChooser new];
    a.accessoryView = [chooser viewWithAllowed:MD_HOST_ACCESS_CAPS initial:mask lifetime:NO];
    [a addButtonWithTitle:@"Cancel"]; [a addButtonWithTitle:@"Save Exact Capabilities"];
    [a beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (!self->_closed && response == NSAlertSecondButtonReturn) [self->_link command:MDPolicyCommand(s,p,c,chooser.selected,NO,nil)];
    }];
}
@end