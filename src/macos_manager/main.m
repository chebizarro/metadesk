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

@interface MDManagerApp ()
@property(nonatomic) BOOL smokeQuit;
@end

@implementation MDManagerApp {
    NSWindow *_window;
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
        NSMenu *menu=[NSMenu new]; NSMenuItem *appItem=[NSMenuItem new]; [menu addItem:appItem];
        appItem.submenu=[[NSMenu alloc] initWithTitle:@"Metadesk Manager"];
        [appItem.submenu addItemWithTitle:@"Quit Metadesk Manager" action:@selector(terminate:) keyEquivalent:@"q"];
        app.mainMenu=menu;
        __attribute__((objc_precise_lifetime)) MDManagerApp *delegate=[MDManagerApp new];
        delegate.smokeQuit = argc == 2 && !strcmp(argv[1], "--smoke-quit");
        app.delegate=delegate; [app run];
    }
    return 0;
}
#endif
