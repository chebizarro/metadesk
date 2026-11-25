#import <AppKit/AppKit.h>
#import "model.h"
#import "access_page.h"
#import "hosts_page.h"
#import "identity_page.h"
#include <signal.h>

static void AddTab(NSTabView *tabs, NSString *title, NSView *view) {
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title; item.view = view; [tabs addTabViewItem:item];
}

@interface Manager : NSObject <NSApplicationDelegate, NSTabViewDelegate>
@end
@implementation Manager {
    NSWindow *_window;
    NSTextField *_address, *_relay, *_routes;
    MDHostsPage *_hostsPage;
    MDLocalHostPages *_local;
    MDIdentityPage *_identity;
    dispatch_queue_t _previewQueue;
    dispatch_source_t _terminate;
    NSTimer *_terminationDeadline;
    BOOL _terminationPending, _terminationReplied;
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
    _previewQueue=dispatch_queue_create("metadesk.manager.relay-preview",DISPATCH_QUEUE_SERIAL);
    NSTabView *tabs=[NSTabView new];
    _hostsPage=[MDHostsPage new]; _local=[MDLocalHostPages new]; _identity=[[MDIdentityPage alloc] initWithHosts:_hostsPage];
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
    __weak Manager *weakSelf=self;
    dispatch_source_set_registration_handler(_terminate, ^{ [weakSelf showWindow]; });
    dispatch_activate(_terminate);
}
- (void)showWindow {
    [_window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
}
- (void)tabView:(NSTabView *)tabs didSelectTabViewItem:(NSTabViewItem *)item {
    (void)tabs;
    if ([item.identifier isEqual:@"Hosts"]) [_hostsPage activate];
    if ([item.identifier isEqual:@"Identity"]) [_identity activate];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { (void)app; return YES; }
- (void)finishTermination {
    if (_terminationReplied) return;
    _terminationReplied=YES;
    [_terminationDeadline invalidate]; _terminationDeadline=nil;
    [NSApp replyToApplicationShouldTerminate:YES];
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app;
    if (!_previewQueue || _terminationReplied) return NSTerminateNow;
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
        __weak Manager *weakSelf=self;
        _terminationDeadline=[NSTimer timerWithTimeInterval:5 repeats:NO block:^(NSTimer *timer) {
            (void)timer; [weakSelf finishTermination];
        }];
        [NSRunLoop.mainRunLoop addTimer:_terminationDeadline forMode:NSRunLoopCommonModes];
        [NSRunLoop.mainRunLoop addTimer:_terminationDeadline forMode:NSModalPanelRunLoopMode];
    }
    return NSTerminateLater;
}
@end

int main(void) {
    signal(SIGPIPE,SIG_IGN); signal(SIGTERM,SIG_IGN);
    @autoreleasepool {
        NSApplication *app=NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu *menu=[NSMenu new]; NSMenuItem *appItem=[NSMenuItem new]; [menu addItem:appItem];
        appItem.submenu=[[NSMenu alloc] initWithTitle:@"Metadesk Manager"];
        [appItem.submenu addItemWithTitle:@"Quit Metadesk Manager" action:@selector(terminate:) keyEquivalent:@"q"];
        app.mainMenu=menu;
        __attribute__((objc_precise_lifetime)) Manager *delegate=[Manager new];
        app.delegate=delegate; [app run];
    }
    return 0;
}
