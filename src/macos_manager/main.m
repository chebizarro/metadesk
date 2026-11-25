#import <AppKit/AppKit.h>
#import "model.h"
#include <signal.h>

static NSButton *Button(NSString *title, id target, SEL action) {
    return [NSButton buttonWithTitle:title target:target action:action];
}

static NSTextField *Field(NSStackView *page, NSString *placeholder) {
    NSTextField *field = [NSTextField textFieldWithString:@""];
    field.placeholderString = placeholder;
    field.accessibilityLabel = placeholder;
    [page addArrangedSubview:field];
    [field.widthAnchor constraintEqualToAnchor:page.widthAnchor].active = YES;
    return field;
}

static NSTextView *Output(NSStackView *page, NSString *initial) {
    NSScrollView *scroll = [NSTextView scrollableTextView];
    NSTextView *output = (NSTextView *)scroll.documentView;
    output.editable = NO;
    output.selectable = YES;
    output.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    output.textContainerInset = NSMakeSize(8, 8);
    output.string = initial;
    [page addArrangedSubview:scroll];
    [scroll.widthAnchor constraintEqualToAnchor:page.widthAnchor].active = YES;
    [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:90].active = YES;
    return output;
}

static NSStackView *Page(NSTabView *tabs, NSString *title) {
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    NSView *container = [[NSView alloc] init];
    NSStackView *stack = [[NSStackView alloc] init];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    [container addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:container.topAnchor constant:14],
        [stack.bottomAnchor constraintEqualToAnchor:container.bottomAnchor constant:-14],
        [stack.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:14],
        [stack.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-14],
    ]];
    item.view = container;
    [tabs addTabViewItem:item];
    return stack;
}

static NSString *PermissionText(MDPermission state) {
    switch (state) {
    case MDPermissionGranted: return @"Allowed";
    case MDPermissionMissing: return @"Not allowed";
    default: return @"Unknown";
    }
}

@interface Manager : NSObject <NSApplicationDelegate>
@end

@implementation Manager {
    NSWindow *_window;
    NSTextField *_endpoint, *_stateDirectory, *_address, *_relay;
    NSTextView *_host, *_hosts, *_identity, *_routes, *_access, *_activity;
    dispatch_queue_t _worker;
    NSUInteger _pendingJobs;
    BOOL _terminationPending;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    _worker = dispatch_queue_create("metadesk.macos-manager", DISPATCH_QUEUE_SERIAL);
    NSTabView *tabs = [[NSTabView alloc] init];
    NSStackView *page = Page(tabs, @"Hosts");
    _stateDirectory = Field(page, @"Default cache, or enter an absolute path");
    [page addArrangedSubview:Button(@"Read Cached Hosts", self, @selector(loadHosts:))];
    _hosts = Output(page, @"Reads the client role’s committed host keys using its external signer. "
                         "No relay sync or connect action in this prototype.");

    page = Page(tabs, @"This Computer");
    _endpoint = Field(page, @"Absolute path to the host’s management socket");
    NSStackView *buttons = [NSStackView stackViewWithViews:@[
        Button(@"Recheck Host", self, @selector(loadHost:)),
        Button(@"Screen Recording Settings", self, @selector(openScreenSettings:)),
        Button(@"Accessibility Settings", self, @selector(openAXSettings:)),
    ]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    [page addArrangedSubview:buttons];
    _host = Output(page, @"");

    page = Page(tabs, @"Access");
    _access = Output(page, @"");

    page = Page(tabs, @"Identity");
    [page addArrangedSubview:Button(@"Read Saved Roles", self, @selector(loadAccounts:))];
    _identity = Output(page, @"Shows saved public account metadata and role assignments, not live signer status. "
                             "Use the account CLI to make changes.");

    page = Page(tabs, @"Relays");
    _address = Field(page, @"Host npub, nprofile or hex public key");
    _relay = Field(page, @"Explicit bootstrap URL (optional)");
    [page addArrangedSubview:Button(@"Preview", self, @selector(preview:))];
    _routes = Output(page, @"Offline identity and bootstrap preview. No default relay, network lookup, or connection.");

    page = Page(tabs, @"Activity");
    _activity = Output(page, @"");
    [self showSnapshot:nil path:nil];

    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 820, 540)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    _window.releasedWhenClosed = NO;
    _window.title = @"Metadesk Manager — Prototype";
    _window.contentMinSize = NSMakeSize(650, 410);
    _window.contentView = tabs;
    [tabs selectTabViewItemAtIndex:1];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app;
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app;
    if (!_pendingJobs) return NSTerminateNow;
    _terminationPending = YES;
    return NSTerminateLater;
}

/* Queue and completion accounting belongs to AppKit's main thread. Wait for
 * every queued job to release its IPC/signer/store resources before quitting. */
- (void)runJob:(id (^)(void))work completion:(void (^)(id))completion {
    ++_pendingJobs;
    dispatch_async(_worker, ^{
        id result = work();
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(result);
            if (--self->_pendingJobs == 0 && self->_terminationPending)
                [NSApp replyToApplicationShouldTerminate:YES];
        });
    });
}

- (void)read:(NSString *(^)(void))work output:(NSTextView *)output sender:(NSButton *)button {
    if (_terminationPending) return;
    button.enabled = NO;
    output.string = @"Reading…";
    [self runJob:work completion:^(id result) { output.string = result; button.enabled = YES; }];
}

- (void)loadAccounts:(NSButton *)button {
    [self read:^{ return MDAccountsSummary(); } output:_identity sender:button];
}

- (void)loadHosts:(NSButton *)button {
    NSString *directory = [_stateDirectory.stringValue copy];
    [self read:^{ return MDCachedHosts(directory); } output:_hosts sender:button];
}

- (void)preview:(NSButton *)button {
    (void)button;
    _routes.string = MDRoutePreview(_address.stringValue, _relay.stringValue);
}

- (void)loadHost:(NSButton *)button {
    if (_terminationPending) return;
    NSString *path = [_endpoint.stringValue copy];
    button.enabled = NO;
    _endpoint.enabled = NO;
    _host.string = @"Checking the host…";
    _access.string = _activity.string = @"Checking the host…";
    [self runJob:^{ return MDHostRead(path); } completion:^(id snapshot) {
        [self showSnapshot:snapshot path:path];
        self->_endpoint.enabled = button.enabled = YES;
    }];
}

- (void)showSnapshot:(NSDictionary *)snapshot path:(NSString *)path {
    id host = snapshot[@"host"];
    id state = [host isKindOfClass:NSDictionary.class] ? host[@"state"] : nil;
    NSString *source = path.length ? [NSString stringWithFormat:@"Last check: %@", path] : @"Enter the running host’s socket, then select Recheck Host.";
    NSString *lifecycle = [state isKindOfClass:NSString.class] ? state : @"Unavailable";
    _host.string = [NSString stringWithFormat:@"%@\nHost status: %@ (not a capture readiness check)\n\n"
        "Screen Recording — %@\nAllows the host to capture its display for video sharing.\n\n"
        "Accessibility — %@\nAllows the host to read the accessibility tree and perform authorized actions. It does not test pointer or keyboard delivery.\n\n"
        "Open the matching pane, or navigate to System Settings › Privacy & Security and select Screen Recording "
        "(Screen & System Audio Recording on newer versions) or Accessibility. Grant access to the responsible host identity, "
        "not this manager. Restart the host if macOS requires it, then select Recheck Host.\n\n"
        "Unknown means the host could not report that permission. Allowed does not enable sharing or grant a remote client access.",
        source, lifecycle, PermissionText(MDHostPermission(snapshot, MDScreenRecording)), PermissionText(MDHostPermission(snapshot, MDAccessibility))];
    id requests = snapshot[@"requests"];
    NSString *count = [requests isKindOfClass:NSArray.class] ? [NSString stringWithFormat:@"%lu", (unsigned long)[requests count]] : @"Unknown";
    _access.string = [NSString stringWithFormat:@"%@\n\nRequest inbox entries: %@\nUse the host CLI to review or decide requests and manage remembered clients.", source, count];
    id session = snapshot[@"session"];
    NSString *summary = @"Session state unavailable.";
    if (session == NSNull.null) summary = @"No session reservation in this snapshot.";
    else if ([session isKindOfClass:NSDictionary.class]) {
        id peer = session[@"peer"];
        NSString *key = [peer isKindOfClass:NSString.class] ? MDPublicIdentity([peer UTF8String]) : nil;
        summary = [NSString stringWithFormat:@"A session slot is occupied. This does not prove a stream is bound.\n%@", key ?: @"Peer unavailable."];
    }
    _activity.string = [NSString stringWithFormat:@"%@\n\n%@", source, summary];
}

- (void)openSettings:(MDPermissionKind)kind {
    if (![NSWorkspace.sharedWorkspace openURL:MDPermissionSettingsURL(kind)]) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"Could not open the privacy pane";
        alert.informativeText = @"Open System Settings › Privacy & Security and choose the pane manually.";
        [alert beginSheetModalForWindow:_window completionHandler:nil];
    }
}

- (void)openScreenSettings:(NSButton *)button { (void)button; [self openSettings:MDScreenRecording]; }
- (void)openAXSettings:(NSButton *)button { (void)button; [self openSettings:MDAccessibility]; }
@end

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu *menu = [[NSMenu alloc] init];
        NSMenuItem *appItem = [[NSMenuItem alloc] init];
        [menu addItem:appItem];
        appItem.submenu = [[NSMenu alloc] initWithTitle:@"Metadesk Manager"];
        [appItem.submenu addItemWithTitle:@"Quit Metadesk Manager" action:@selector(terminate:) keyEquivalent:@"q"];
        app.mainMenu = menu;
        Manager *delegate = [[Manager alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
