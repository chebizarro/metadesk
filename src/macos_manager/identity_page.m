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
static const MdSignerType ExternalBackends[] = {MD_SIGNER_NIP5F, MD_SIGNER_NIP46, MD_SIGNER_NIP55L};
static NSString *BackendTitle(MdSignerType backend) {
    switch (backend) {
        case MD_SIGNER_NIP5F: return @"Local socket signer (NIP-5F)";
        case MD_SIGNER_NIP46: return @"Remote signer (NIP-46)";
        case MD_SIGNER_NIP55L: return @"D-Bus signer (NIP-55L)";
        default: return @"Unsupported signer";
    }
}
static NSTextField *Entry(NSString *label, NSString *value) {
    NSTextField *field = [NSTextField textFieldWithString:value ?: @""];
    field.placeholderString = label; field.accessibilityLabel = label;
    [field.widthAnchor constraintEqualToConstant:400].active = YES;
    return field;
}
@interface MDIdentityPage () {
    MDHostsPage *_hosts;
    NSString *_directory;
    MDIdentityConnect _connectSigner;
    MdCredentialStore *(^_credentials)(void);
    dispatch_queue_t _queue;
    dispatch_group_t _drain;
    atomic_bool _cancelled;
    NSArray<NSDictionary *> *_accounts;
    NSPopUpButton *_list;
    NSTextField *_status, *_details, *_roles;
    NSButton *_refresh, *_client, *_host, *_unbindClient, *_unbindHost, *_remove, *_fips, *_cancel, *_add, *_label, *_pair;
    BOOL _closed, _busy, _cancellable;
}
- (BOOL)cancelled;
@end
static BOOL Cancelled(void *context) { return [(__bridge MDIdentityPage *)context cancelled]; }

@implementation MDIdentityPage
- (instancetype)initWithHosts:(MDHostsPage *)hosts {
    return [self initWithHosts:hosts directory:nil connect:NULL credentials:nil];
}
- (instancetype)initWithHosts:(MDHostsPage *)hosts directory:(NSString *)directory
                      connect:(MDIdentityConnect)connect credentials:(MdCredentialStore *(^)(void))credentials {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _hosts = hosts; _queue = dispatch_queue_create("metadesk.manager.identity",DISPATCH_QUEUE_SERIAL);
        _directory=[directory copy]; _connectSigner=connect;
        if (credentials) _credentials=[credentials copy];
        else _credentials=^{ return md_credential_store_default(); };
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
    _add=Button(@"Add Signer Account…",self,@selector(add:));
    _label=Button(@"Edit Label…",self,@selector(editLabel:));
    _pair=Button(@"Import Pairing…",self,@selector(importPairing:));
    _remove=Button(@"Remove Account…",self,@selector(remove:)); _cancel=Button(@"Cancel Verification",self,@selector(cancel:));
    _fips=[NSButton checkboxWithTitle:@"Host will use FIPS (verify the local node key)" target:nil action:NULL]; _fips.state=NSControlStateValueOn;
    NSStackView *content = Stack(@[],NO);
    for (NSView *v in @[_status,_roles,_list,_details,_refresh,_fips,Stack(@[_client,_host],YES),Stack(@[_unbindClient,_unbindHost],YES),
        Stack(@[_add,_label,_pair],YES), Stack(@[_remove,_cancel],YES),
        Label(@"Create keys, unlock, and authorize operations in your external signer. Metadesk has no password. Restart an already-running host after changing its role.")]) {
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
    MdSignerType backend = [a[@"backend"] intValue];
    BOOL bindable=a && MDIdentitySupportsBackend(backend);
    BOOL any=NO;
    for (size_t i=0; i<sizeof(ExternalBackends)/sizeof(*ExternalBackends); ++i)
        any |= MDIdentitySupportsBackend(ExternalBackends[i]);
    _add.enabled=ready && any; _label.enabled=ready && a;
    _pair.enabled=ready && bindable && backend==MD_SIGNER_NIP46;
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
            NSArray *accounts=MDIdentityAccounts(self->_directory,&readError);
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
        MdCredentialStore *store=self->_credentials();
        NSString *error=MDIdentityBind(self->_directory,store,role,a,expected,fips,self->_connectSigner,NULL,Cancelled,(__bridge void *)self);
        md_credential_store_destroy(store);
        return error.length ? error : role==MD_ACCOUNT_HOST ? @"Host role saved. Restart the host to adopt it." : @"Client role saved. Reloading Hosts.";
    }];
}
- (void)bindClient:(id)sender { (void)sender; if (_client.enabled) [self bind:MD_ACCOUNT_CLIENT account:self.selected]; }
- (void)bindHost:(id)sender { (void)sender; if (_host.enabled) [self bind:MD_ACCOUNT_HOST account:self.selected]; }
- (void)unbindClient:(id)sender { (void)sender; [self bind:MD_ACCOUNT_CLIENT account:nil]; }
- (void)unbindHost:(id)sender { (void)sender; [self bind:MD_ACCOUNT_HOST account:nil]; }
- (void)cancel:(id)sender { (void)sender; atomic_store(&_cancelled,true); _status.stringValue=@"Cancelling verification. Waiting for the signer operation to drain…"; [self selection:nil]; }
- (NSWindow *)sheetWindow {
    NSWindow *window = self.view.window;
    return !_busy && !_closed && !window.attachedSheet ? window : nil;
}
- (void)add:(id)sender {
    (void)sender;
    NSWindow *window = [self sheetWindow];
    if (!window || !_add.enabled) return;
    NSPopUpButton *backend = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (size_t i=0; i<sizeof(ExternalBackends)/sizeof(*ExternalBackends); ++i) {
        MdSignerType choice = ExternalBackends[i];
        if (!MDIdentitySupportsBackend(choice)) continue;
        [backend addItemWithTitle:BackendTitle(choice)]; backend.lastItem.tag = choice;
    }
    backend.accessibilityLabel = @"Signer backend";
    NSTextField *key = Entry(@"Public key (npub or lowercase hex)", nil);
    NSTextField *label = Entry(@"Public label (optional)", nil);
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Add Signer Account";
    alert.informativeText = @"Use the public key of an existing account. This does not contact the signer or create a key.";
    NSStackView *form = Stack(@[backend,key,label],NO); form.frame = NSMakeRect(0,0,400,90); alert.accessoryView = form;
    [alert addButtonWithTitle:@"Add Account"]; [alert addButtonWithTitle:@"Cancel"];
    alert.window.initialFirstResponder = key;
    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse result) {
        NSString *publicKey=key.stringValue, *publicLabel=label.stringValue;
        key.stringValue=@""; label.stringValue=@"";
        if (result!=NSAlertFirstButtonReturn || self->_closed) return;
        MdSignerType type = (MdSignerType)backend.selectedTag;
        [self run:@"Adding account…" suspend:NO cancellable:NO work:^NSString *{
            NSString *error = MDIdentityAdd(self->_directory,type,publicKey,publicLabel);
            return error.length ? error : @"Public account added. Select it to assign a role.";
        }];
    }];
}
- (void)editLabel:(id)sender {
    (void)sender;
    NSDictionary *account=self.selected;
    NSWindow *window = [self sheetWindow];
    if (!window || !account) return;
    NSTextField *label = Entry(@"Public account label", account[@"label"]);
    label.frame = NSMakeRect(0,0,400,24);
    NSAlert *alert = [NSAlert new]; alert.messageText = @"Edit Public Label";
    alert.informativeText = MDHostNpub(account[@"key"]);
    alert.accessoryView = label;
    [alert addButtonWithTitle:@"Save Label"]; [alert addButtonWithTitle:@"Cancel"];
    alert.window.initialFirstResponder = label;
    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse result) {
        NSString *value=label.stringValue; label.stringValue=@"";
        if (result!=NSAlertFirstButtonReturn || self->_closed) return;
        [self run:@"Saving public label…" suspend:NO cancellable:NO work:^NSString *{
            NSString *error = MDIdentityLabel(self->_directory,account,value);
            return error.length ? error : @"Public label saved.";
        }];
    }];
}
- (void)choosePairingFile:(NSWindow *)window completion:(void (^)(NSURL *))completion {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.resolvesAliases = NO;
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        completion(response == NSModalResponseOK ? panel.URL : nil);
    }];
}
- (void)importPairing:(id)sender {
    (void)sender;
    NSDictionary *account=self.selected;
    NSWindow *window = [self sheetWindow];
    if (!window || !_pair.enabled || !account) return;
    NSAlert *alert = [NSAlert new]; alert.messageText = @"Import Saved NIP-46 Pairing";
    alert.informativeText = [NSString stringWithFormat:@"Choose a saved pairing JSON file from your trusted signer for:\n%@\n\nThis replaces the Keychain pairing, not the signing key or role. The selected file contains credentials and will not be deleted or modified.", MDHostNpub(account[@"key"])];
    [alert addButtonWithTitle:@"Choose Pairing File…"]; [alert addButtonWithTitle:@"Cancel"];
    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse result) {
        if (result!=NSAlertFirstButtonReturn || self->_closed || self->_busy) return;
        [self choosePairingFile:window completion:^(NSURL *url) {
            if (!url || self->_closed || self->_busy) return;
            [self run:@"Importing pairing into Keychain…" suspend:[account[@"client"] boolValue] cancellable:NO work:^NSString *{
                MdCredentialStore *store=self->_credentials();
                NSString *error=MDIdentityImportPairingFile(self->_directory,store,account,url,Cancelled,(__bridge void *)self);
                md_credential_store_destroy(store);
                return error.length ? error : @"Pairing saved in Keychain. Verify the signer before assigning a role.";
            }];
        }];
    }];
}
- (void)remove:(id)sender {
    (void)sender; NSDictionary *account=self.selected;
    if (!account || _busy || _closed || !self.view.window || self.view.window.attachedSheet) return;
    NSAlert *a=[NSAlert new]; a.messageText=@"Remove Signer Account?";
    a.informativeText=@"This removes the saved account and role assignments, and any Metadesk-held remote pairing. It does not delete the key in the external signer.";
    [a addButtonWithTitle:@"Cancel"]; [a addButtonWithTitle:@"Remove Account"];
    [a beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (response!=NSAlertSecondButtonReturn || self->_closed) return;
        [self run:@"Removing the account…" suspend:YES cancellable:NO work:^NSString *{
            MdCredentialStore *store=self->_credentials();
            NSString *error=MDIdentityRemove(self->_directory,store,account);
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