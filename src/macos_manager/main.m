#import "manager_app.h"
#import "model.h"
#import "access_page.h"
#import "hosts_page.h"
#import "hosts_launch.h"
#import "identity_page.h"
#include <signal.h>

static void AddTab(NSTabView *tabs, NSString *title, NSView *view) {
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title; item.view = view; [tabs addTabViewItem:item];
}

static NSString *const MDHelpURL = @"https://github.com/chebizarro/metadesk/blob/master/docs/manager-macos.md";
static NSString *const MDIssuesURL = @"https://github.com/chebizarro/metadesk/issues";

static NSMenuItem *Item(NSMenu *menu, NSString *title, SEL action, NSString *key, NSEventModifierFlags mask) {
    NSMenuItem *item = [menu addItemWithTitle:title action:action keyEquivalent:key ?: @""];
    if (key.length) item.keyEquivalentModifierMask = mask;
    return item;
}
static NSMenu *Submenu(NSMenu *bar, NSString *title) {
    NSMenuItem *holder = [bar addItemWithTitle:title action:NULL keyEquivalent:@""];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
    holder.submenu = menu;
    return menu;
}

@interface MDManagerApp ()
@property(nonatomic) BOOL smokeQuit;
@end

@implementation MDManagerApp {
    NSWindow *_window;
    NSTabView *_tabs;
    NSTextField *_address, *_relay, *_routes;
    MDHostsPage *_hostsPage;
    MDLocalHostPages *_local;
    MDIdentityPage *_identity;
    dispatch_queue_t _previewQueue;
    dispatch_source_t _terminate;
    NSTimer *_terminationDeadline;
    BOOL _terminationPending, _terminationReplied, _smokeReady;
}
- (instancetype)initWithHosts:(MDHostsPage *)hosts local:(MDLocalHostPages *)local identity:(MDIdentityPage *)identity {
    if ((self=[super init])) {
        _hostsPage=hosts; _local=local; _identity=identity;
        _previewQueue=dispatch_queue_create("metadesk.manager.relay-preview",DISPATCH_QUEUE_SERIAL);
    }
    return self;
}
- (instancetype)init { return [self initWithHosts:nil local:nil identity:nil]; }

/* Standard macOS menu bar. Every item has a nil target so it travels the
 * responder chain: the field editor handles editing, the window handles
 * window commands, and this delegate handles section and page commands. */
+ (NSMenu *)makeMainMenu {
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand, shift = NSEventModifierFlagShift,
        opt = NSEventModifierFlagOption, ctrl = NSEventModifierFlagControl;
    NSString *app = @"Metadesk Manager";
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@"Main Menu"];

    NSMenu *m = Submenu(bar, app);
    Item(m, [@"About " stringByAppendingString:app], @selector(orderFrontStandardAboutPanel:), nil, 0);
    [m addItem:NSMenuItem.separatorItem];
    NSMenu *services = [[NSMenu alloc] initWithTitle:@"Services"];
    Item(m, @"Services", NULL, nil, 0).submenu = services;
    [m addItem:NSMenuItem.separatorItem];
    Item(m, [@"Hide " stringByAppendingString:app], @selector(hide:), @"h", cmd);
    Item(m, @"Hide Others", @selector(hideOtherApplications:), @"h", cmd | opt);
    Item(m, @"Show All", @selector(unhideAllApplications:), nil, 0);
    [m addItem:NSMenuItem.separatorItem];
    Item(m, [@"Quit " stringByAppendingString:app], @selector(terminate:), @"q", cmd);

    m = Submenu(bar, @"File");
    Item(m, @"Connect to Host…", @selector(connectToHost:), @"k", cmd);
    Item(m, @"Scan QR Code Image…", @selector(scanQR:), nil, 0);
    [m addItem:NSMenuItem.separatorItem];
    Item(m, @"Import Hosts…", @selector(importHosts:), nil, 0);
    Item(m, @"Export Hosts…", @selector(exportHosts:), nil, 0);
    [m addItem:NSMenuItem.separatorItem];
    Item(m, @"Close Window", @selector(performClose:), @"w", cmd);

    m = Submenu(bar, @"Edit");
    Item(m, @"Undo", @selector(undo:), @"z", cmd);
    Item(m, @"Redo", @selector(redo:), @"z", cmd | shift);
    [m addItem:NSMenuItem.separatorItem];
    Item(m, @"Cut", @selector(cut:), @"x", cmd);
    Item(m, @"Copy", @selector(copy:), @"c", cmd);
    Item(m, @"Paste", @selector(paste:), @"v", cmd);
    Item(m, @"Delete", @selector(delete:), nil, 0);
    Item(m, @"Select All", @selector(selectAll:), @"a", cmd);
    [m addItem:NSMenuItem.separatorItem];
    NSMenu *find = [[NSMenu alloc] initWithTitle:@"Find"];
    Item(m, @"Find", NULL, nil, 0).submenu = find;
    Item(find, @"Find Hosts…", @selector(findHosts:), @"f", cmd);

    m = Submenu(bar, @"View");
    NSArray<NSString *> *sections = MDManagerApp.sectionTitles;
    for (NSUInteger i = 0; i < sections.count; ++i) {
        NSMenuItem *item = Item(m, sections[i], @selector(showSection:), [NSString stringWithFormat:@"%lu", (unsigned long)i + 1], cmd);
        item.representedObject = sections[i];
    }
    [m addItem:NSMenuItem.separatorItem];
    Item(m, @"Refresh", @selector(refresh:), @"r", cmd);
    [m addItem:NSMenuItem.separatorItem];
    Item(m, @"Show Sidebar", @selector(toggleSidebar:), @"s", cmd | ctrl);
    Item(m, @"Enter Full Screen", @selector(toggleFullScreen:), @"f", cmd | ctrl);

    NSMenu *window = Submenu(bar, @"Window");
    Item(window, @"Minimize", @selector(performMiniaturize:), @"m", cmd);
    Item(window, @"Zoom", @selector(performZoom:), nil, 0);
    [window addItem:NSMenuItem.separatorItem];
    Item(window, @"Bring All to Front", @selector(arrangeInFront:), nil, 0);

    NSMenu *help = Submenu(bar, @"Help");
    Item(help, [app stringByAppendingString:@" Help"], @selector(openHelp:), @"?", cmd);
    Item(help, @"Report an Issue…", @selector(reportIssue:), nil, 0);
    return bar;
}
+ (NSArray<NSString *> *)sectionTitles { return @[@"Hosts", @"This Computer", @"Access", @"Identity", @"Relays"]; }
+ (void)installMainMenu:(NSApplication *)app {
    NSMenu *bar = [self makeMainMenu];
    app.mainMenu = bar;
    app.servicesMenu = [bar itemAtIndex:0].submenu.itemArray[2].submenu;
    app.windowsMenu = [bar itemWithTitle:@"Window"].submenu;
    app.helpMenu = [bar itemWithTitle:@"Help"].submenu;
}
- (NSView *)relays {
    _address = [NSTextField textFieldWithString:@""]; _address.placeholderString = @"Host npub, nprofile or hex public key";
    _relay = [NSTextField textFieldWithString:@""]; _relay.placeholderString = @"Explicit bootstrap URL (optional)";
    _routes = [NSTextField wrappingLabelWithString:@"Offline identity and bootstrap preview. This tab does not change Hosts relay choices."];
    NSButton *preview = [NSButton buttonWithTitle:@"Preview" target:self action:@selector(preview:)];
    NSStackView *stack = [NSStackView stackViewWithViews:@[_address,_relay,preview,_routes]];
    stack.translatesAutoresizingMaskIntoConstraints = NO; stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment=NSLayoutAttributeLeading; stack.spacing=8;
    for (NSView *v in @[_address,_relay,_routes]) [v.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active=YES;
    NSView *root=[NSView new]; [root addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[[stack.topAnchor constraintEqualToAnchor:root.topAnchor constant:14],
        [stack.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:14], [stack.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-14]]];
    return root;
}
- (void)preview:(NSButton *)sender {
    if (_terminationPending) return;
    NSString *address=_address.stringValue,*relay=_relay.stringValue;
    sender.enabled=NO;
    dispatch_async(_previewQueue, ^{ @autoreleasepool {
        NSString *text=MDRoutePreview(address,relay);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!self->_terminationPending) { self->_routes.stringValue=text; sender.enabled=YES; }
        });
    } });
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    NSTabView *tabs=[NSTabView new];
    if (!_hostsPage) _hostsPage=[MDHostsPage new];
    if (!_local) {
        // Render the real local pages without querying notification settings.
        _local = self.smokeQuit ? [[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) {
            return [[MDHostLink alloc] initWithUpdate:update];
        }] : [MDLocalHostPages new];
    }
    if (!_identity) _identity=[[MDIdentityPage alloc] initWithHosts:_hostsPage];
    AddTab(tabs,@"Hosts",_hostsPage.view);
    AddTab(tabs,@"This Computer",_local.computerView);
    AddTab(tabs,@"Access",_local.accessView);
    AddTab(tabs,@"Identity",_identity.view);
    AddTab(tabs,@"Relays",[self relays]);
    NSView *activity=[NSView new];
    NSTextField *note=[NSTextField labelWithString:@"Live session status and disconnection are in This Computer. No activity history is stored here."];
    note.translatesAutoresizingMaskIntoConstraints=NO; [activity addSubview:note];
    [note.leadingAnchor constraintEqualToAnchor:activity.leadingAnchor constant:14].active=YES;
    [note.topAnchor constraintEqualToAnchor:activity.topAnchor constant:14].active=YES;
    AddTab(tabs,@"Activity",activity);
    _window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,820,540)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    _window.releasedWhenClosed=NO; _window.title=@"Metadesk Manager"; _window.restorable=NO;
    _window.contentMinSize=NSMakeSize(650,410); _window.contentView=tabs;
    [tabs selectTabViewItemAtIndex:1]; tabs.delegate=self;
    _tabs=tabs;
    [_window center];
    /* Publish readiness (the visible window) only once a terminal stop can be
     * observed. SIGTERM uses the same asynchronous drain path as Quit. */
    _terminate=dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,SIGTERM,0,dispatch_get_main_queue());
    dispatch_source_set_event_handler(_terminate, ^{
        /* Begin the AppKit termination loop from its run loop, not inside a
         * GCD main-queue handler: drain completions also target that queue. */
        CFRunLoopRef loop=CFRunLoopGetMain();
        CFRunLoopPerformBlock(loop,kCFRunLoopCommonModes,^{ [NSApp terminate:nil]; });
        CFRunLoopWakeUp(loop);
    });
    __weak MDManagerApp *weakSelf=self;
    dispatch_source_set_registration_handler(_terminate, ^{ [weakSelf showWindow]; });
    dispatch_activate(_terminate);
}
- (void)showWindow {
    [_window makeKeyAndOrderFront:nil];
    if (!self.smokeQuit) { [NSApp activateIgnoringOtherApps:YES]; return; }
    // Wait for an AppKit run-loop turn after showing the window. Then use
    // the normal termination delegate (including page and worker drain).
    [NSTimer scheduledTimerWithTimeInterval:0 repeats:NO block:^(NSTimer *timer) {
        (void)timer;
        if (!NSApp.isRunning || !NSScreen.screens.count || !self->_window.isVisible) {
            fputs("Manager smoke: no visible window in a running AppKit session\n", stderr);
            exit(EXIT_FAILURE);
        }
        self->_smokeReady = YES;
        [NSApp terminate:nil];
    }];
}
- (NSString *)currentSection { return _tabs.selectedTabViewItem.identifier; }
- (void)showSection:(id)sender {
    NSString *section = [sender isKindOfClass:NSString.class] ? sender : [sender representedObject];
    if ([_tabs indexOfTabViewItemWithIdentifier:section] != NSNotFound) [_tabs selectTabViewItemWithIdentifier:section];
}
/* Commands that act on the Hosts page work from any section. */
- (BOOL)forwardToHosts:(SEL)action from:(id)sender {
    if (_terminationPending || !_hostsPage) return NO;
    [self showSection:@"Hosts"];
    return [NSApp sendAction:action to:_hostsPage from:sender];
}
- (void)connectToHost:(id)sender { if ([self forwardToHosts:@selector(focusHostEntry:) from:sender]) return; }
- (void)scanQR:(id)sender { [self forwardToHosts:@selector(scanQR:) from:sender]; }
- (void)importHosts:(id)sender { [self forwardToHosts:@selector(importHosts:) from:sender]; }
- (void)exportHosts:(id)sender { [self forwardToHosts:@selector(exportHosts:) from:sender]; }
- (void)findHosts:(id)sender { [self forwardToHosts:@selector(focusSearch:) from:sender]; }
- (void)refresh:(id)sender {
    NSString *section = self.currentSection;
    if ([section isEqual:@"Hosts"]) [NSApp sendAction:@selector(refresh:) to:_hostsPage from:sender];
    else if ([section isEqual:@"Identity"]) [NSApp sendAction:@selector(refresh:) to:_identity from:sender];
    else if ([section isEqual:@"This Computer"] || [section isEqual:@"Access"]) [_local refreshSection:section];
}
- (void)openHelp:(id)sender { (void)sender; [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:MDHelpURL]]; }
- (void)reportIssue:(id)sender { (void)sender; [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:MDIssuesURL]]; }
- (BOOL)validateMenuItem:(NSMenuItem *)item { return [self validateUserInterfaceItem:item]; }
- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item {
    SEL action = item.action;
    if (_terminationPending) return action == @selector(openHelp:) || action == @selector(reportIssue:);
    if (action == @selector(showSection:)) {
        if ([(id)item isKindOfClass:NSMenuItem.class])
            ((NSMenuItem *)item).state = [[(NSMenuItem *)item representedObject] isEqual:self.currentSection] ? NSControlStateValueOn : NSControlStateValueOff;
        return _window != nil;
    }
    if (action == @selector(refresh:)) {
        NSString *section = self.currentSection;
        if ([section isEqual:@"Hosts"]) return [_hostsPage validateUserInterfaceItem:item];
        if ([section isEqual:@"Identity"]) return [_identity validateUserInterfaceItem:item];
        if ([section isEqual:@"This Computer"] || [section isEqual:@"Access"]) return [_local canRefreshSection:section];
        return NO;
    }
    SEL hosts[] = {@selector(connectToHost:), @selector(scanQR:), @selector(importHosts:), @selector(exportHosts:), @selector(findHosts:)};
    SEL forwarded[] = {@selector(focusHostEntry:), @selector(scanQR:), @selector(importHosts:), @selector(exportHosts:), @selector(focusSearch:)};
    for (size_t i = 0; i < sizeof(hosts) / sizeof(*hosts); ++i) if (action == hosts[i]) {
        if (!_hostsPage || !_window) return NO;
        NSMenuItem *probe = [[NSMenuItem alloc] initWithTitle:@"" action:forwarded[i] keyEquivalent:@""];
        return [_hostsPage validateUserInterfaceItem:probe];
    }
    return [self respondsToSelector:action];
}
- (void)tabView:(NSTabView *)tabs didSelectTabViewItem:(NSTabViewItem *)item {
    (void)tabs;
    if (self.smokeQuit) return; // Don’t read or sync host/account state.
    if ([item.identifier isEqual:@"Hosts"]) [_hostsPage activate];
    if ([item.identifier isEqual:@"Identity"]) [_identity activate];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { (void)app; return YES; }
- (void)finishTermination {
    if (_terminationReplied) return;
    _terminationReplied=YES;
    [_terminationDeadline invalidate]; _terminationDeadline=nil;
    [self replyToTermination];
}
- (void)replyToTermination { [NSApp replyToApplicationShouldTerminate:YES]; }
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    if (self.smokeQuit && _smokeReady && _terminationReplied)
        puts("PASS: Manager window opened and normal quit completed");
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app;
    if (!_hostsPage || !_local || !_identity || _terminationReplied) return NSTerminateNow;
    if (!_terminationPending) {
        _terminationPending=YES;
        dispatch_group_t group=dispatch_group_create();
        dispatch_group_enter(group); [_hostsPage close:^{ dispatch_group_leave(group); }];
        dispatch_group_enter(group); [_identity close:^{ dispatch_group_leave(group); }];
        dispatch_group_enter(group); [_local close:^{ dispatch_group_leave(group); }];
        dispatch_group_async(group,_previewQueue,^{});
        dispatch_group_notify(group,dispatch_get_main_queue(), ^{ [self finishTermination]; });
        /* A signer call may be unable to interrupt its transport. Cancellation
         * invalidates the pending role save, but do not hold logout indefinitely
         * for the call to return. This timer also runs in the termination loop. */
        __weak MDManagerApp *weakSelf=self;
        _terminationDeadline=[NSTimer timerWithTimeInterval:5 repeats:NO block:^(NSTimer *timer) {
            (void)timer;
            MDManagerApp *app = weakSelf;
            if (app.smokeQuit) {
                fputs("Manager smoke: quit did not drain\n", stderr);
                exit(EXIT_FAILURE);
            }
            [app finishTermination];
        }];
        [NSRunLoop.mainRunLoop addTimer:_terminationDeadline forMode:NSRunLoopCommonModes];
        [NSRunLoop.mainRunLoop addTimer:_terminationDeadline forMode:NSModalPanelRunLoopMode];
    }
    return NSTerminateLater;
}
@end

#ifndef MD_MACOS_MANAGER_APP_TEST
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        // An offline installation diagnostic; use the same resolver as Connect.
        if (argc == 2 && !strcmp(argv[1], "--print-client-path")) {
            NSBundle *bundle = NSBundle.mainBundle;
            NSString *client = MDHostClientPath(bundle.executablePath, bundle.bundlePath,
                NSProcessInfo.processInfo.environment[@"PATH"]);
            if (!client) { fputs("No executable metadesk-client found; the Manager app does not search PATH\n", stderr); return 1; }
            puts(client.fileSystemRepresentation);
            return 0;
        }
        signal(SIGPIPE,SIG_IGN); signal(SIGTERM,SIG_IGN);
        NSApplication *app=NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [MDManagerApp installMainMenu:app];
        __attribute__((objc_precise_lifetime)) MDManagerApp *delegate=[MDManagerApp new];
        delegate.smokeQuit = argc == 2 && !strcmp(argv[1], "--smoke-quit");
        app.delegate=delegate; [app run];
    }
    return 0;
}
#endif
