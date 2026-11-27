#import "manager_app.h"
#import "model.h"
#import "access_page.h"
#import "hosts_page.h"
#import "hosts_launch.h"
#import "identity_page.h"
#import "main_window.h"
#import "ui.h"
#include <signal.h>

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
    MDMainWindowController *_main;
    NSTextField *_address, *_relay, *_routes;
    NSButton *_preview;
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
    Item(m, @"Scan QR Code…", @selector(scanQR:), nil, 0);
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
/* Offline route preview: never contacts a relay or changes Hosts relays. */
- (NSView *)relays {
    _address = MDKeyField(@"npub, nprofile or hex public key", @"Host identity");
    _relay = MDKeyField(@"wss://relay.example.com (optional)", @"Bootstrap relay");
    _preview = MDButton(@"Preview", self, @selector(preview:));
    _address.target = self; _address.action = @selector(preview:);
    _relay.target = self; _relay.action = @selector(preview:);
    _routes = MDWrappingValue(@"");
    _routes.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
    _routes.accessibilityLabel = @"Preview result";
    NSGridView *form = MDForm(@[@[@"Host identity", _address], @[@"Bootstrap relay", _relay], @[NSNull.null, _preview]]);
    NSStackView *stack = nil;
    NSView *settings = [_hostsPage relaySettingsView];
    NSStackView *result = MDRow(@[MDFormLabel(@"Result"), _routes]);
    NSView *page = MDPage(@[settings, MDSeparator(), MDHeader(@"Test a Host Identity"),
        MDNote(@"See which key and bootstrap relay a host identity resolves to, without contacting any relay or changing your relays."),
        form, result], &stack);
    NSMutableArray *forms = [NSMutableArray arrayWithObject:form];
    NSMutableArray *queue = [NSMutableArray arrayWithObject:settings];
    while (queue.count) { NSView *v = queue.lastObject; [queue removeLastObject];
        if ([v isKindOfClass:NSGridView.class]) [forms addObject:v]; else [queue addObjectsFromArray:v.subviews]; }
    MDAlignForms(forms);
    [stack setCustomSpacing:MDGroupSpacing afterView:settings];
    [stack setCustomSpacing:MDGroupSpacing afterView:stack.arrangedSubviews[1]];
    [stack setCustomSpacing:MDRowSpacing afterView:form];
    _routes.stringValue = @"Enter a host identity and choose Preview.";
    return MDScrollingPage(page);
}
- (void)preview:(id)sender {
    (void)sender;
    if (_terminationPending || !_preview.enabled) return;
    NSString *address=MDHostTrim(_address.stringValue),*relay=MDHostTrim(_relay.stringValue);
    NSButton *button=_preview;
    button.enabled=NO;
    dispatch_async(_previewQueue, ^{ @autoreleasepool {
        NSString *text=MDRoutePreview(address,relay);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!self->_terminationPending) { self->_routes.stringValue=text; button.enabled=YES; }
        });
    } });
}
- (NSArray<NSString *> *)sectionIdentifiers { return MDManagerApp.sectionTitles; }
- (NSValue *)defaultContentSize { return [NSValue valueWithSize:MDMainWindowController.defaultContentSize]; }
/* Builds the pages and the main window once. The snapshot renderer calls this
 * directly; launching calls it from applicationDidFinishLaunching:. */
- (NSWindow *)makeMainWindow {
    if (_main) return _main.window;
    if (!_hostsPage) _hostsPage=[MDHostsPage new];
    if (!_local) {
        // Render the real local pages without querying notification settings.
        _local = self.smokeQuit ? [[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) {
            return [[MDHostLink alloc] initWithUpdate:update];
        }] : [MDLocalHostPages new];
    }
    if (!_identity) _identity=[[MDIdentityPage alloc] initWithHosts:_hostsPage];
    MDSection *hosts=[MDSection section:@"Hosts" symbol:@"network" controller:_hostsPage view:nil];
    hosts.refreshHelp=@"Sync the address book with its relays"; hosts.searchable=YES;
    MDSection *computer=[MDSection section:@"This Computer" symbol:@"desktopcomputer" controller:nil view:_local.computerView];
    computer.refreshHelp=@"Recheck the host’s permissions";
    MDSection *access=[MDSection section:@"Access" symbol:@"lock.shield" controller:nil view:_local.accessView];
    access.refreshHelp=@"Reload remembered clients";
    MDSection *identity=[MDSection section:@"Identity" symbol:@"person.text.rectangle" controller:_identity view:nil];
    identity.refreshHelp=@"Reload saved accounts";
    (void)_hostsPage.view;
    NSViewController *relays=[NSViewController new]; relays.view=[self relays];
    MDSection *preview=[MDSection section:@"Relays" symbol:@"antenna.radiowaves.left.and.right" controller:relays view:nil];
    _main=[[MDMainWindowController alloc] initWithSections:@[hosts,computer,access,identity,preview]];
    __weak MDManagerApp *weakSelf=self;
    _main.sectionChanged=^(NSString *section) { [weakSelf sectionChanged:section]; };
    _main.searchChanged=^(NSString *text) { [weakSelf searchChanged:text]; };
    _local.pendingChanged=^(NSUInteger count) {
        MDManagerApp *app=weakSelf;
        if (!app) return; /* a late timer tick after the app delegate is gone */
        [app->_main setBadge:count ? [NSString stringWithFormat:@"%lu", (unsigned long)count] : nil forSection:@"Access"];
    };
    [_main showSection:@"This Computer"];
    return _main.window;
}
- (void)sectionChanged:(NSString *)section {
    if (self.smokeQuit) return; // Don’t read or sync host/account state.
    if ([section isEqual:@"Hosts"]) [_hostsPage activate];
    if ([section isEqual:@"Identity"]) [_identity activate];
    if ([section isEqual:@"This Computer"] || [section isEqual:@"Access"]) [_local activate];
}
- (void)searchChanged:(NSString *)text { [_hostsPage setSearchText:text]; }
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    [self makeMainWindow];
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
    NSWindow *window=_main.window;
    [window makeKeyAndOrderFront:nil];
    if (!self.smokeQuit) { [NSApp activateIgnoringOtherApps:YES]; return; }
    // Wait for an AppKit run-loop turn after showing the window. Then use
    // the normal termination delegate (including page and worker drain).
    [NSTimer scheduledTimerWithTimeInterval:0 repeats:NO block:^(NSTimer *timer) {
        (void)timer;
        if (!NSApp.isRunning || !NSScreen.screens.count || !window.isVisible) {
            fputs("Manager smoke: no visible window in a running AppKit session\n", stderr);
            exit(EXIT_FAILURE);
        }
        self->_smokeReady = YES;
        [NSApp terminate:nil];
    }];
}
- (NSString *)currentSection { return _main.currentSection; }
- (void)showSection:(id)sender {
    NSString *section = [sender isKindOfClass:NSString.class] ? sender : [sender representedObject];
    if (!_main || ![MDManagerApp.sectionTitles containsObject:section]) return;
    [_main showSection:section];
    [_main.window makeKeyAndOrderFront:nil];
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
- (void)findHosts:(id)sender {
    (void)sender;
    if (_terminationPending || !_main) return;
    [self showSection:@"Hosts"];
    [_main focusSearch];
}
- (void)refresh:(id)sender {
    NSString *section = self.currentSection;
    if ([section isEqual:@"Hosts"]) [NSApp sendAction:@selector(refresh:) to:_hostsPage from:sender];
    else if ([section isEqual:@"Identity"]) [NSApp sendAction:@selector(refresh:) to:_identity from:sender];
    else if ([section isEqual:@"This Computer"] || [section isEqual:@"Access"]) [_local refreshSection:section];
}
/* First-run step from Hosts: open Identity and start adding an account. */
- (void)addSignerAccount:(id)sender {
    (void)sender;
    if (_terminationPending || !_main) return;
    [self showSection:@"Identity"];
    [_identity beginAddAccount];
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
        return _main != nil;
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
        if (!_hostsPage || !_main) return NO;
        NSMenuItem *probe = [[NSMenuItem alloc] initWithTitle:@"" action:forwarded[i] keyEquivalent:@""];
        return [_hostsPage validateUserInterfaceItem:probe];
    }
    return [self respondsToSelector:action];
}
- (BOOL)validateToolbarItem:(NSToolbarItem *)item { return [self validateUserInterfaceItem:item]; }
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
