#import "identity_page.h"
#import "identity_model.h"
#import "hosts_page.h"
#import "hosts_model.h"
#include <stdatomic.h>

static NSTextField *Label(NSString *text) { return [NSTextField wrappingLabelWithString:text ?: @""]; }
static NSButton *Button(NSString *title,id target,SEL action) { return [NSButton buttonWithTitle:title target:target action:action]; }
static NSStackView *Stack(NSArray *views,BOOL row) {
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = row ? NSUserInterfaceLayoutOrientationHorizontal : NSUserInterfaceLayoutOrientationVertical;
    s.alignment = row ? NSLayoutAttributeCenterY : NSLayoutAttributeLeading; s.spacing = 8; return s;
}
@interface MDIdentityPage () {
    MDHostsPage *_hosts;
    dispatch_queue_t _queue;
    dispatch_group_t _drain;
    atomic_bool _cancelled;
    NSArray<NSDictionary *> *_accounts;
    NSPopUpButton *_list;
    NSTextField *_status, *_details, *_roles;
    NSButton *_refresh, *_client, *_host, *_unbindClient, *_unbindHost, *_remove, *_fips, *_cancel;
    BOOL _closed, _busy, _cancellable;
}
- (BOOL)cancelled;
@end
static BOOL Cancelled(void *context) { return [(__bridge MDIdentityPage *)context cancelled]; }

@implementation MDIdentityPage
- (instancetype)initWithHosts:(MDHostsPage *)hosts {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _hosts = hosts; _queue = dispatch_queue_create("metadesk.manager.identity",DISPATCH_QUEUE_SERIAL);
        _drain = dispatch_group_create(); atomic_init(&_cancelled,false);
    }
    return self;
}
- (BOOL)cancelled { return atomic_load(&_cancelled); }
- (void)loadView {
    _list = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO]; _list.target=self; _list.action=@selector(selection:);
    [_list setAccessibilityLabel:@"Signer accounts"];
    _status=Label(@"Load saved signer accounts."); _details=Label(nil); _roles=Label(nil);
    _refresh=Button(@"Reload Accounts",self,@selector(refresh:));
    _client=Button(@"Use for Client",self,@selector(bindClient:)); _host=Button(@"Use for Host",self,@selector(bindHost:));
    _unbindClient=Button(@"Unassign Client",self,@selector(unbindClient:)); _unbindHost=Button(@"Unassign Host",self,@selector(unbindHost:));
    _remove=Button(@"Remove Account…",self,@selector(remove:)); _cancel=Button(@"Cancel Verification",self,@selector(cancel:));
    _fips=[NSButton checkboxWithTitle:@"Host will use FIPS (verify the local node key)" target:nil action:NULL]; _fips.state=NSControlStateValueOn;
    NSStackView *content = Stack(@[],NO);
    for (NSView *v in @[_status,_roles,_list,_details,_refresh,_fips,Stack(@[_client,_host],YES),Stack(@[_unbindClient,_unbindHost],YES),
        Stack(@[_remove,_cancel],YES), Label(@"Add public account metadata or saved NIP-46 pairing with the account CLI. Unlock or authorize the external signer there; Metadesk has no password. Restart an already-running host after changing its role.")]) {
        [content addArrangedSubview:v]; [v.widthAnchor constraintLessThanOrEqualToAnchor:content.widthAnchor].active=YES;
    }
    NSView *view=[NSView new]; [view addSubview:content]; content.translatesAutoresizingMaskIntoConstraints=NO;
    [NSLayoutConstraint activateConstraints:@[[content.topAnchor constraintEqualToAnchor:view.topAnchor constant:14],
        [content.leadingAnchor constraintEqualToAnchor:view.leadingAnchor constant:14], [content.trailingAnchor constraintEqualToAnchor:view.trailingAnchor constant:-14]]];
    self.view=view; [self render];
}
- (void)activate { if (!_accounts && !_busy) [self refresh:nil]; }
- (NSDictionary *)selected { return _list.selectedItem.representedObject; }
- (NSDictionary *)accountInRole:(MdAccountRole)role {
    NSString *field=role==MD_ACCOUNT_CLIENT?@"client":@"host";
    for (NSDictionary *a in _accounts) if ([a[field] boolValue]) return a;
    return nil;
}
- (void)render {
    NSString *selected=self.selected[@"id"];
    [_list removeAllItems];
    for (NSDictionary *a in _accounts) {
        NSString *title =[NSString stringWithFormat:@"%@ · %@", [a[@"label"] length]?a[@"label"]:MDHostFingerprint(a[@"key"]),a[@"transport"]];
        NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""]; item.representedObject=a;
        [_list.menu addItem:item]; if ([selected isEqual:a[@"id"]]) [_list selectItem:item];
    }
    if (!_list.numberOfItems) [_list addItemWithTitle:@"No saved accounts"];
    _roles.stringValue=[NSString stringWithFormat:@"Client: %@\nHost: %@",MDHostNpub([self accountInRole:MD_ACCOUNT_CLIENT][@"key"])?:@"Not assigned",
        MDHostNpub([self accountInRole:MD_ACCOUNT_HOST][@"key"])?:@"Not assigned"];
    [self selection:nil];
}
- (void)selection:(id)sender {
    (void)sender;
    NSDictionary *a=self.selected;
    _details.stringValue=a?[NSString stringWithFormat:@"%@\nFingerprint: %@\n%@",MDHostNpub(a[@"key"]),MDHostFingerprint(a[@"key"]),a[@"transport"]]:@"";
    BOOL ready=!_closed && !_busy;
    BOOL bindable=a && [a[@"backend"] intValue]!=MD_SIGNER_DIRECT_KEY;
    _refresh.enabled=ready; _client.enabled=_host.enabled=ready && bindable;
    _unbindClient.enabled=ready && [self accountInRole:MD_ACCOUNT_CLIENT]!=nil;
    _unbindHost.enabled=ready && [self accountInRole:MD_ACCOUNT_HOST]!=nil;
    _fips.enabled=ready; _remove.enabled=ready && a;
    _cancel.hidden=!_busy || !_cancellable; _cancel.enabled=!self.cancelled;
}
- (void)run:(NSString *)notice suspend:(BOOL)suspend cancellable:(BOOL)cancellable work:(NSString *(^)(void))work {
    if (_closed || _busy) return;
    atomic_store(&_cancelled,false); _busy=YES; _cancellable=cancellable;
    _status.stringValue=notice; [self selection:nil];
    dispatch_group_enter(_drain);
    dispatch_block_t begin=^{
        dispatch_async(self->_queue, ^{ @autoreleasepool {
            NSString *result=work ? work() : @"";
            NSString *readError=nil;
            NSArray *accounts=MDIdentityAccounts(nil,&readError);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!self->_closed) {
                    self->_accounts=accounts; self->_busy=NO;
                    self->_status.stringValue=result.length ? result : readError ?: @"Saved accounts loaded.";
                    [self render];
                    if (suspend) [self->_hosts resumeAfterRoleEdit];
                }
                dispatch_group_leave(self->_drain);
            });
        } });
    };
    if (suspend) [_hosts suspendForRoleEdit:begin]; else begin();
}
- (void)refresh:(id)sender { (void)sender; [self run:@"Loading accounts…" suspend:NO cancellable:NO work:nil]; }
- (void)bind:(MdAccountRole)role account:(NSDictionary *)a {
    if (_busy || _closed) return;
    BOOL fips=_fips.state==NSControlStateValueOn;
    NSString *expected=[self accountInRole:role][@"id"]?:@"";
    NSString *notice=!a?@"Unassigning role…":[a[@"backend"] intValue]==MD_SIGNER_NIP46 ?
        @"Waiting for remote signer verification or approval. You can cancel the binding." : @"Verifying the selected signer…";
    [self run:notice suspend:role==MD_ACCOUNT_CLIENT cancellable:a!=nil work:^NSString *{
        MdCredentialStore *store=md_credential_store_default();
        NSString *error=MDIdentityBind(nil,store,role,a,expected,fips,NULL,NULL,Cancelled,(__bridge void *)self);
        md_credential_store_destroy(store);
        return error.length ? error : role==MD_ACCOUNT_HOST ? @"Host role saved. Restart the host to adopt it." : @"Client role saved. Reloading Hosts.";
    }];
}
- (void)bindClient:(id)sender { (void)sender; [self bind:MD_ACCOUNT_CLIENT account:self.selected]; }
- (void)bindHost:(id)sender { (void)sender; [self bind:MD_ACCOUNT_HOST account:self.selected]; }
- (void)unbindClient:(id)sender { (void)sender; [self bind:MD_ACCOUNT_CLIENT account:nil]; }
- (void)unbindHost:(id)sender { (void)sender; [self bind:MD_ACCOUNT_HOST account:nil]; }
- (void)cancel:(id)sender { (void)sender; atomic_store(&_cancelled,true); _status.stringValue=@"Cancelling verification. Waiting for the signer operation to drain…"; [self selection:nil]; }
- (void)remove:(id)sender {
    (void)sender; NSDictionary *account=self.selected;
    if (!account || _busy || _closed || !self.view.window || self.view.window.attachedSheet) return;
    NSAlert *a=[NSAlert new]; a.messageText=@"Remove Signer Account?";
    a.informativeText=@"This removes the saved account and role assignments, and any Metadesk-held remote pairing. It does not delete the key in the external signer.";
    [a addButtonWithTitle:@"Cancel"]; [a addButtonWithTitle:@"Remove Account"];
    [a beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (response!=NSAlertSecondButtonReturn || self->_closed) return;
        [self run:@"Removing the account…" suspend:YES cancellable:NO work:^NSString *{
            MdCredentialStore *store=md_credential_store_default();
            NSString *error=MDIdentityRemove(nil,store,account);
            md_credential_store_destroy(store);
            return error.length ? error : @"Account removed. Restart a running host if it used this role.";
        }];
    }];
}
- (void)close:(dispatch_block_t)drained {
    _closed=YES; atomic_store(&_cancelled,true);
    dispatch_group_notify(_drain,dispatch_get_main_queue(),drained);
}
@end