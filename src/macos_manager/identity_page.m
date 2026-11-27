#import "identity_page.h"
#import "identity_model.h"
#import "hosts_page.h"
#import "hosts_model.h"
#import "ui.h"
#include <stdatomic.h>

static NSString *const HostSaved = @"Host role saved. Restart the host to use it.";
static NSString *const ClientSaved = @"Client role saved. Hosts is reloading with the new client key.";
static NSString *const AccountAdded = @"Account added. Select it to assign a role.";
static NSString *const LabelSaved = @"Label saved.";
static NSString *const PairingSaved = @"Pairing saved in Keychain. Assigning a role will still ask the signer to prove the key.";
static NSString *const AccountRemoved = @"Account removed. Restart a running host if it used this account.";
/* Worker results are either the model's error message or one of these. */
static BOOL success(NSString *message) {
    return [@[HostSaved, ClientSaved, AccountAdded, LabelSaved, PairingSaved, AccountRemoved] containsObject:message];
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
/* What this build lacks for an account it can't use. */
static NSString *MissingSupport(MdSignerType backend) {
    switch (backend) {
        case MD_SIGNER_NIP5F: return @"local socket (NIP-5F) signers";
        case MD_SIGNER_NIP46: return @"remote (NIP-46) signers";
        case MD_SIGNER_NIP55L: return @"D-Bus (NIP-55L) signers";
        default: return @"this signer type";
    }
}
@interface MDIdentityPage () <NSTableViewDataSource, NSTableViewDelegate> {
    MDHostsPage *_hosts;
    NSString *_directory;
    MDIdentityConnect _connectSigner;
    MdCredentialStore *(^_credentials)(void);
    dispatch_queue_t _queue;
    dispatch_group_t _drain;
    atomic_bool _cancelled;
    NSArray<NSDictionary *> *_accounts;
    NSTableView *_list;
    MDStatusView *_status;
    NSTextField *_unsupported; /* why an account's signer can't be used by this build */
    NSTextField *_details, *_clientRole, *_hostRole, *_emptyTitle, *_emptyDetail, *_roleHint;
    NSView *_empty;
    NSString *_selectedID;
    NSButton *_client, *_host, *_unbindClient, *_unbindHost, *_remove, *_fips, *_cancel, *_add, *_label, *_pair;
    BOOL _closed, _busy, _cancellable, _pendingAdd, _reloading;
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
    _clientRole = MDValue(@""); _hostRole = MDValue(@"");
    _unbindClient = MDButton(@"Unassign", self, @selector(unbindClient:));
    _unbindHost = MDButton(@"Unassign", self, @selector(unbindHost:));
    _unbindClient.accessibilityLabel = @"Unassign Client"; _unbindHost.accessibilityLabel = @"Unassign Host";
    NSStackView *clientRow = MDRow(@[_clientRole]), *hostRow = MDRow(@[_hostRole]);
    [clientRow addView:_unbindClient inGravity:NSStackViewGravityTrailing];
    [hostRow addView:_unbindHost inGravity:NSStackViewGravityTrailing];
    clientRow.alignment = hostRow.alignment = NSLayoutAttributeCenterY;
    _roleHint = MDNote(@"");
    NSGridView *roles = MDForm(@[@[@"Client", clientRow], @[@"Host", hostRow], @[NSNull.null, _roleHint]]);
    _status = [MDStatusView new];
    _cancel = MDButton(@"Cancel Verification", self, @selector(cancel:));
    NSStackView *statusRow = MDRow(@[_status, _cancel]); statusRow.alignment = NSLayoutAttributeCenterY;

    _list = [MDTable new];
    MDColumnNamed(_list, @"Label", @"Label", 150, nil).minWidth = 80;
    MDColumnNamed(_list, @"Signer", @"Signer", 165, nil);
    MDColumnNamed(_list, @"Fingerprint", @"Fingerprint", 165, nil);
    MDColumnNamed(_list, @"Role", @"Role", 64, nil);
    MDFinishColumns(_list, @"Label");
    _list.dataSource = self; _list.delegate = self; _list.accessibilityLabel = @"Signer accounts";
    _list.allowsEmptySelection = YES;
    NSView *empty = nil; NSTextField *title = nil, *detail = nil;
    NSView *area = MDTableArea(MDTableScroll(_list), &empty, &title, &detail);
    _empty = empty; _emptyTitle = title; _emptyDetail = detail;
    _add = MDButton(@"Add Account…", self, @selector(add:));
    _remove = MDButton(@"Remove…", self, @selector(remove:));
    _add.toolTip = @"Add the public key of an account in your external signer";
    _label = MDButton(@"Edit Label…", self, @selector(editLabel:));
    _pair = MDButton(@"Import Pairing…", self, @selector(importPairing:));
    _pair.toolTip = @"Restore a saved NIP-46 pairing for the selected remote signer account";
    NSStackView *listActions = MDRow(@[]); listActions.alignment = NSLayoutAttributeCenterY;
    for (NSView *v in @[_add, _label, _pair]) [listActions addView:v inGravity:NSStackViewGravityLeading];
    [listActions addView:_remove inGravity:NSStackViewGravityTrailing];
    _details = MDValue(@""); _details.font = [NSFont monospacedSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
    _details.accessibilityLabel = @"Selected account public key";
    _client = MDButton(@"Use for Client", self, @selector(bindClient:));
    _host = MDButton(@"Use for Host", self, @selector(bindHost:));
    _fips = [NSButton checkboxWithTitle:@"Check the local FIPS node key when assigning the host" target:nil action:NULL];
    _fips.state = NSControlStateValueOn;
    _fips.toolTip = @"Turn off only if the host will be reached by direct IP rather than FIPS.";
    NSStackView *assign = MDRow(@[_client, _host]); assign.alignment = NSLayoutAttributeCenterY;
    _unsupported = MDNote(@"");
    NSGridView *selected = MDForm(@[@[@"Public key", _details], @[@"Assign", assign], @[NSNull.null, _unsupported], @[NSNull.null, _fips]]);
    NSStackView *stack = nil;
    NSView *page = MDPage(@[MDHeader(@"Roles"), roles, statusRow, MDSeparator(), MDHeader(@"Signer Accounts"), area, listActions,
                         selected, MDNote(@"Create keys, unlock and approve requests in your external signer; Metadesk has no password. "
                                          @"Restart a running host after changing its role.")], &stack);
    MDAlignForms(@[roles, selected]);
    [stack setCustomSpacing:MDGroupSpacing afterView:statusRow];
    [stack setCustomSpacing:MDGroupSpacing afterView:stack.arrangedSubviews[3]];
    [stack setCustomSpacing:6 afterView:area];
    [stack setCustomSpacing:MDGroupSpacing afterView:listActions];
    [stack setCustomSpacing:MDGroupSpacing afterView:selected];
    self.view = MDScrollingPage(page);
    [_status setText:@"Load saved signer accounts." kind:MDStatusInfo];
    [self render];
}
- (void)activate { if (!_accounts && !_busy) [self refresh:nil]; }
- (NSDictionary *)selected {
    NSInteger row = _list.selectedRow;
    return row >= 0 && (NSUInteger)row < _accounts.count ? _accounts[(NSUInteger)row] : nil;
}
static NSString *AccountTitle(NSDictionary *a) { return [a[@"label"] length] ? a[@"label"] : MDHostFingerprint(a[@"key"]); }
static NSString *SignerTitle(NSDictionary *a) {
    MdSignerType backend = [a[@"backend"] intValue];
    return backend == MD_SIGNER_NIP5F || backend == MD_SIGNER_NIP46 || backend == MD_SIGNER_NIP55L ? BackendTitle(backend) : a[@"transport"];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { (void)table; return (NSInteger)_accounts.count; }
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    NSDictionary *a = _accounts[(NSUInteger)row];
    NSString *identifier = column.identifier;
    if ([identifier isEqual:@"Label"]) {
        NSTableCellView *cell = MDCell(table, @"LabelCell", [a[@"label"] length] ? a[@"label"] : @"No label");
        cell.textField.textColor = [a[@"label"] length] ? NSColor.labelColor : NSColor.secondaryLabelColor;
        return cell;
    }
    if ([identifier isEqual:@"Signer"]) return MDCell(table, @"SignerCell", SignerTitle(a));
    if ([identifier isEqual:@"Fingerprint"]) {
        NSTableCellView *cell = MDCell(table, @"FingerprintCell", MDHostFingerprint(a[@"key"]));
        cell.textField.font = [NSFont monospacedSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
        cell.textField.toolTip = MDHostNpub(a[@"key"]);
        return cell;
    }
    NSMutableArray *roles = [NSMutableArray array];
    if ([a[@"client"] boolValue]) [roles addObject:@"Client"];
    if ([a[@"host"] boolValue]) [roles addObject:@"Host"];
    NSTableCellView *cell = MDCell(table, @"RoleCell", roles.count ? [roles componentsJoinedByString:@", "] : @"—");
    cell.textField.textColor = roles.count ? NSColor.labelColor : NSColor.secondaryLabelColor;
    return cell;
}
- (NSString *)tableView:(NSTableView *)table typeSelectStringForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table; (void)column; return AccountTitle(_accounts[(NSUInteger)row]);
}
- (void)tableViewSelectionDidChange:(NSNotification *)note {
    (void)note;
    if (!_reloading) [self selection:nil];
}
- (NSString *)clientRoleKey { return _accounts ? [self accountInRole:MD_ACCOUNT_CLIENT][@"key"] ?: @"" : nil; }
- (NSString *)hostRoleKey { return _accounts ? [self accountInRole:MD_ACCOUNT_HOST][@"key"] ?: @"" : nil; }
- (NSDictionary *)accountInRole:(MdAccountRole)role {
    NSString *field=role==MD_ACCOUNT_CLIENT?@"client":@"host";
    for (NSDictionary *a in _accounts) if ([a[field] boolValue]) return a;
    return nil;
}
- (void)render {
    /* Restore the selection by account ID, never by row index: another writer
     * may have inserted or reordered accounts. A vanished account is not
     * replaced by its neighbour. */
    NSString *selected = _selectedID;
    _reloading = YES;
    [_list reloadData];
    NSUInteger index = NSNotFound;
    for (NSUInteger i = 0; selected && i < _accounts.count; ++i) if ([_accounts[i][@"id"] isEqual:selected]) { index = i; break; }
    if (!selected && _accounts.count) index = 0;
    if (index == NSNotFound) _selectedID = nil;
    if (index != NSNotFound) [_list selectRowIndexes:[NSIndexSet indexSetWithIndex:index] byExtendingSelection:NO];
    else [_list deselectAll:nil];
    _reloading = NO;
    _empty.hidden = _accounts.count != 0;
    _emptyTitle.stringValue = _accounts ? @"No Signer Accounts" : @"Accounts Not Loaded";
    _emptyDetail.stringValue = _accounts ? @"Choose Add Account to save the public key of an account in your external signer." : @"Saved accounts load when this page opens.";
    NSDictionary *client = [self accountInRole:MD_ACCOUNT_CLIENT], *host = [self accountInRole:MD_ACCOUNT_HOST];
    _clientRole.stringValue = client ? [NSString stringWithFormat:@"%@ — %@", AccountTitle(client), MDHostNpub(client[@"key"])] : @"Not assigned";
    _hostRole.stringValue = host ? [NSString stringWithFormat:@"%@ — %@", AccountTitle(host), MDHostNpub(host[@"key"])] : @"Not assigned";
    _clientRole.toolTip = client ? MDHostNpub(client[@"key"]) : nil; _hostRole.toolTip = host ? MDHostNpub(host[@"key"]) : nil;
    _clientRole.textColor = client ? NSColor.labelColor : NSColor.secondaryLabelColor;
    _hostRole.textColor = host ? NSColor.labelColor : NSColor.secondaryLabelColor;
    _roleHint.stringValue = client ? @"" : _accounts.count ? @"To read your host list and connect, select an account below and choose Use for Client."
                                                           : @"Add an account below, then choose Use for Client to read your host list and connect.";
    MDSetRowHidden(_roleHint, !_roleHint.stringValue.length || !_accounts);
    [self selection:nil];
}
- (void)selection:(id)sender {
    (void)sender;
    NSDictionary *a=self.selected;
    _selectedID = a[@"id"];
    _details.stringValue = a ? MDHostNpub(a[@"key"]) ?: @"" : @"Select an account.";
    _details.toolTip = a ? MDHostNpub(a[@"key"]) : nil;
    _details.textColor = a ? NSColor.labelColor : NSColor.secondaryLabelColor;
    BOOL ready=!_closed && !_busy;
    MdSignerType backend = [a[@"backend"] intValue];
    BOOL bindable=a && MDIdentitySupportsBackend(backend);
    BOOL any=NO;
    for (size_t i=0; i<sizeof(ExternalBackends)/sizeof(*ExternalBackends); ++i)
        any |= MDIdentitySupportsBackend(ExternalBackends[i]);
    _add.enabled=ready && any; _label.enabled=ready && a;
    /* A signer type this build doesn't include is never offered: Import
     * Pairing appears only for NIP-46 accounts in a build with NIP-46, and
     * the note says why roles can't be assigned to such an account. */
    _pair.enabled=ready && bindable && backend==MD_SIGNER_NIP46;
    _pair.hidden=!a || backend!=MD_SIGNER_NIP46 || !MDIdentitySupportsBackend(MD_SIGNER_NIP46);
    _unsupported.stringValue = a && !bindable ? [NSString stringWithFormat:
        @"This copy of Metadesk Manager was built without support for %@, so it can’t assign a role to this account.", MissingSupport(backend)] : @"";
    MDSetRowHidden(_unsupported, !_unsupported.stringValue.length);
    _client.enabled=_host.enabled=ready && bindable;
    _list.enabled=ready;
    _unbindClient.enabled=ready && [self accountInRole:MD_ACCOUNT_CLIENT]!=nil;
    _unbindHost.enabled=ready && [self accountInRole:MD_ACCOUNT_HOST]!=nil;
    _fips.enabled=ready; _remove.enabled=ready && a;
    _cancel.hidden=!_busy || !_cancellable; _cancel.enabled=!self.cancelled;
}
- (void)run:(NSString *)notice suspend:(BOOL)suspend cancellable:(BOOL)cancellable work:(NSString *(^)(void))work {
    if (_closed || _busy) return;
    atomic_store(&_cancelled,false); _busy=YES; _cancellable=cancellable;
    [_status setText:notice kind:MDStatusWorking]; [self selection:nil];
    dispatch_group_enter(_drain);
    dispatch_block_t begin=^{
        dispatch_async(self->_queue, ^{ @autoreleasepool {
            NSString *result=work ? work() : @"";
            NSString *readError=nil;
            NSArray *accounts=MDIdentityAccounts(self->_directory,&readError);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!self->_closed) {
                    self->_accounts=accounts; self->_busy=NO;
                    BOOL failed = readError.length || (result.length && !success(result));
                    [self->_status setText:result.length ? result : readError ?: @"Saved accounts loaded." kind:failed ? MDStatusError : MDStatusOK];
                    [self render];
                    if (suspend) [self->_hosts resumeAfterRoleEdit];
                    if (self->_pendingAdd) { self->_pendingAdd = NO; [self add:nil]; }
                    if (self.accountsChanged) self.accountsChanged();
                }
                dispatch_group_leave(self->_drain);
            });
        } });
    };
    if (suspend) [_hosts suspendForRoleEdit:begin]; else begin();
}
- (void)beginAddAccount {
    if (_closed) return;
    if (!_busy && self.view.window && !self.view.window.attachedSheet && _accounts) [self add:nil];
    else { _pendingAdd = YES; [self activate]; }
}
- (void)refresh:(id)sender { (void)sender; [self run:@"Loading accounts…" suspend:NO cancellable:NO work:nil]; }
- (BOOL)validateMenuItem:(NSMenuItem *)item { return [self validateUserInterfaceItem:item]; }
- (BOOL)validateToolbarItem:(NSToolbarItem *)item { return [self validateUserInterfaceItem:item]; }
- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item {
    if (item.action == @selector(refresh:)) return !_closed && !_busy;
    return [self respondsToSelector:item.action];
}
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
        return error.length ? error : role==MD_ACCOUNT_HOST ? HostSaved : ClientSaved;
    }];
}
- (void)bindClient:(id)sender { (void)sender; if (_client.enabled) [self bind:MD_ACCOUNT_CLIENT account:self.selected]; }
- (void)bindHost:(id)sender { (void)sender; if (_host.enabled) [self bind:MD_ACCOUNT_HOST account:self.selected]; }
- (void)unbindClient:(id)sender { (void)sender; [self bind:MD_ACCOUNT_CLIENT account:nil]; }
- (void)unbindHost:(id)sender { (void)sender; [self bind:MD_ACCOUNT_HOST account:nil]; }
- (void)cancel:(id)sender { (void)sender; atomic_store(&_cancelled,true); [_status setText:@"Cancelling verification. Waiting for the signer to finish…" kind:MDStatusWorking]; [self selection:nil]; }
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
    backend.accessibilityLabel = @"Signer";
    NSTextField *key = MDKeyField(@"npub1… or 64-character hex", @"Public key");
    NSTextField *label = MDKeyField(@"Optional", @"Public label");
    MDStatusView *problem = [MDStatusView new];
    MDSheet *sheet = [[MDSheet alloc] initWithTitle:@"Add Signer Account"
        message:@"Enter the public key of an account that already exists in your external signer. Metadesk saves only the public key; it doesn’t contact the signer or create a key."
          width:500];
    sheet.accessory = MDForm(@[@[@"Signer", backend], @[@"Public key", key], @[@"Label", label], @[NSNull.null, problem]]);
    [problem setText:nil kind:MDStatusNone];
    sheet.initialFirstResponder = key;
    NSButton *addButton = [sheet addButton:@"Add Account" role:MDSheetDefault response:NSAlertFirstButtonReturn];
    [sheet addButton:@"Cancel" role:MDSheetCancel response:NSAlertSecondButtonReturn];
    addButton.enabled = NO;
    __weak NSButton *weakAdd = addButton; __weak MDStatusView *weakProblem = problem;
    id refused = [NSNotificationCenter.defaultCenter addObserverForName:MDSecretRefusedNotification object:key queue:nil usingBlock:^(NSNotification *note) {
        (void)note;
        [weakProblem setText:@"That’s a private key, so it wasn’t entered. Enter the account’s public npub or hex key." kind:MDStatusError];
    }];
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:NSControlTextDidChangeNotification object:key queue:nil usingBlock:^(NSNotification *note) {
        NSTextField *field = note.object;
        NSTextView *editor = (NSTextView *)field.currentEditor;
        if (MDClassifyHostInput(editor ? editor.string : field.stringValue) == MDHostSecret) {
            /* Never keep or echo a secret key; drop the undo that held it. */
            NSUndoManager *undo = editor.undoManager;
            editor.string = @""; field.stringValue = @"";
            [undo removeAllActions]; [field.window.undoManager removeAllActions];
            [weakProblem setText:@"That’s a private key, so it was cleared. Enter the account’s public npub or hex key." kind:MDStatusError];
        } else [weakProblem setText:nil kind:MDStatusNone];
        weakAdd.enabled = MDHostTrim(field.stringValue).length != 0;
    }];
    [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse result) {
        [NSNotificationCenter.defaultCenter removeObserver:observer];
        [NSNotificationCenter.defaultCenter removeObserver:refused];
        NSString *publicKey=key.stringValue, *publicLabel=label.stringValue;
        key.stringValue=@""; label.stringValue=@"";
        if (result!=NSAlertFirstButtonReturn || self->_closed || MDContainsSecretKey(publicKey)) return;
        MdSignerType type = (MdSignerType)backend.selectedTag;
        [self run:@"Adding the account…" suspend:NO cancellable:NO work:^NSString *{
            NSString *error = MDIdentityAdd(self->_directory,type,publicKey,publicLabel);
            return error.length ? error : AccountAdded;
        }];
    }];
}
- (void)editLabel:(id)sender {
    (void)sender;
    NSDictionary *account=self.selected;
    NSWindow *window = [self sheetWindow];
    if (!window || !account) return;
    NSTextField *label = MDKeyField(@"No label", @"Public label");
    label.stringValue = account[@"label"] ?: @"";
    MDSheet *sheet = [[MDSheet alloc] initWithTitle:@"Edit Label"
        message:@"The label is saved with the account’s public key on this Mac. Changing it doesn’t affect its roles." width:480];
    NSTextField *npub = MDKeyValue(MDHostNpub(account[@"key"]) ?: @"");
    npub.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
    sheet.accessory = MDForm(@[@[@"Account", npub], @[@"Label", label]]);
    sheet.initialFirstResponder = label;
    [sheet addButton:@"Save" role:MDSheetDefault response:NSAlertFirstButtonReturn];
    [sheet addButton:@"Cancel" role:MDSheetCancel response:NSAlertSecondButtonReturn];
    [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse result) {
        NSString *value=label.stringValue; label.stringValue=@"";
        if (result!=NSAlertFirstButtonReturn || self->_closed) return;
        [self run:@"Saving the label…" suspend:NO cancellable:NO work:^NSString *{
            NSString *error = MDIdentityLabel(self->_directory,account,value);
            return error.length ? error : LabelSaved;
        }];
    }];
}
- (void)choosePairingFile:(NSWindow *)window completion:(void (^)(NSURL *))completion {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.resolvesAliases = NO;
    panel.message = @"Choose a pairing JSON file (at most 64 KB). Symbolic links aren’t accepted.";
    panel.prompt = @"Import";
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        completion(response == NSModalResponseOK ? panel.URL : nil);
    }];
}
- (void)importPairing:(id)sender {
    (void)sender;
    NSDictionary *account=self.selected;
    NSWindow *window = [self sheetWindow];
    if (!window || !_pair.enabled || !account) return;
    MDSheet *alert = [[MDSheet alloc] initWithTitle:@"Import Remote Signer Pairing"
        message:@"Choose the pairing file your remote signer saved for this account. It replaces the pairing stored in Keychain; it doesn’t change the signing key or assign a role. "
                @"The file contains credentials: keep it safe. Metadesk reads it once and doesn’t change or delete it."
          width:500];
    NSTextField *npub = MDKeyValue(MDHostNpub(account[@"key"]) ?: @"");
    npub.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
    alert.accessory = MDDetails(@[@[@"Account", npub]]);
    [alert addButton:@"Choose File…" role:MDSheetDefault response:NSAlertFirstButtonReturn];
    [alert addButton:@"Cancel" role:MDSheetCancel response:NSAlertSecondButtonReturn];
    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse result) {
        if (result!=NSAlertFirstButtonReturn || self->_closed || self->_busy) return;
        [self choosePairingFile:window completion:^(NSURL *url) {
            if (!url || self->_closed || self->_busy) return;
            [self run:@"Importing the pairing into Keychain…" suspend:[account[@"client"] boolValue] cancellable:NO work:^NSString *{
                MdCredentialStore *store=self->_credentials();
                NSString *error=MDIdentityImportPairingFile(self->_directory,store,account,url,Cancelled,(__bridge void *)self);
                md_credential_store_destroy(store);
                return error.length ? error : PairingSaved;
            }];
        }];
    }];
}
- (void)remove:(id)sender {
    (void)sender; NSDictionary *account=self.selected;
    if (!account || _busy || _closed || !self.view.window || self.view.window.attachedSheet) return;
    NSString *name = [account[@"label"] length] ? MDHostBoundText(account[@"label"], 64) : MDHostFingerprint(account[@"key"]);
    NSAlert *a = MDAlert([NSString stringWithFormat:@"Remove “%@”?", name],
        @"This removes the saved account, any role it holds and any pairing Metadesk stored for it. The key itself stays in your external signer.");
    MDAlertDestructive(a, @"Remove Account");
    [a beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (response!=NSAlertSecondButtonReturn || self->_closed) return;
        [self run:@"Removing the account…" suspend:YES cancellable:NO work:^NSString *{
            MdCredentialStore *store=self->_credentials();
            NSString *error=MDIdentityRemove(self->_directory,store,account);
            md_credential_store_destroy(store);
            return error.length ? error : AccountRemoved;
        }];
    }];
}
- (void)close:(dispatch_block_t)drained {
    _closed=YES; atomic_store(&_cancelled,true);
    dispatch_group_notify(_drain,dispatch_get_main_queue(),drained);
}
@end