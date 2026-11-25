#import "hosts_page.h"
#import "hosts_launch.h"
#include <stdatomic.h>

/* These fields may show encrypted-book text. No selection may be copied,
 * cut, dragged, or offered to Services by their field editor. */
@interface MDPrivateEditor : NSTextView
@end
@implementation MDPrivateEditor
- (void)copy:(id)sender { (void)sender; }
- (void)cut:(id)sender { (void)sender; }
- (NSArray<NSPasteboardType> *)writablePasteboardTypes { return @[]; }
- (BOOL)writeSelectionToPasteboard:(NSPasteboard *)pasteboard type:(NSPasteboardType)type { (void)pasteboard; (void)type; return NO; }
- (BOOL)dragSelectionWithEvent:(NSEvent *)event offset:(NSSize)offset slideBack:(BOOL)slideBack { (void)event; (void)offset; (void)slideBack; return NO; }
- (BOOL)writeSelectionToPasteboard:(NSPasteboard *)pasteboard types:(NSArray<NSPasteboardType> *)types { (void)pasteboard; (void)types; return NO; }
- (id)validRequestorForSendType:(NSPasteboardType)sendType returnType:(NSPasteboardType)returnType {
    return sendType ? nil : [super validRequestorForSendType:sendType returnType:returnType];
}
- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item {
    return item.action != @selector(copy:) && item.action != @selector(cut:) && [super validateUserInterfaceItem:item];
}
@end
@interface MDPrivateFieldCell : NSTextFieldCell
@end
@implementation MDPrivateFieldCell {
    MDPrivateEditor *_editor;
}
- (NSTextView *)fieldEditorForView:(NSView *)control {
    (void)control;
    if (!_editor) {
        _editor = [[MDPrivateEditor alloc] initWithFrame:NSZeroRect];
        _editor.fieldEditor = YES; _editor.allowsUndo = NO; _editor.richText = NO;
        _editor.automaticTextCompletionEnabled = NO;
        _editor.continuousSpellCheckingEnabled = NO;
        _editor.automaticSpellingCorrectionEnabled = NO;
        _editor.automaticQuoteSubstitutionEnabled = NO;
        _editor.automaticDashSubstitutionEnabled = NO;
        _editor.automaticDataDetectionEnabled = NO;
    }
    return _editor;
}
@end

static NSTextField *Field(NSString *placeholder, NSString *value) {
    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 420, 24)];
    MDPrivateFieldCell *cell = [[MDPrivateFieldCell alloc] initTextCell:value ?: @""];
    cell.editable = YES; cell.selectable = YES; cell.bezeled = YES; cell.bordered = YES;
    cell.sendsActionOnEndEditing = NO; cell.placeholderString = placeholder;
    field.cell = cell;
    [field setAccessibilityLabel:placeholder];
    return field;
}
static NSTextField *Label(NSString *value) {
    NSTextField *f = [NSTextField wrappingLabelWithString:value ?: @""];
    f.selectable = NO; return f;
}
static NSButton *Button(NSString *title, id target, SEL action) {
    return [NSButton buttonWithTitle:title target:target action:action];
}
static NSStackView *Stack(NSArray<NSView *> *views, NSUserInterfaceLayoutOrientation orientation) {
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = orientation; s.spacing = 8;
    s.alignment = orientation == NSUserInterfaceLayoutOrientationVertical ? NSLayoutAttributeLeading : NSLayoutAttributeCenterY;
    return s;
}
static void Wide(NSStackView *s, NSView *v) {
    [s addArrangedSubview:v];
    [v.widthAnchor constraintEqualToAnchor:s.widthAnchor constant:-s.edgeInsets.left-s.edgeInsets.right].active = YES;
}
static NSAlert *Alert(NSString *title, NSString *body) {
    NSAlert *a = [NSAlert new]; a.messageText = title; a.informativeText = body; return a;
}
static NSScrollView *TableScroll(NSTableView *table, CGFloat width, CGFloat height) {
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES; scroll.borderType = NSBezelBorder;
    scroll.documentView = table;
    [scroll.heightAnchor constraintEqualToConstant:height].active = YES;
    return scroll;
}

@interface MDHostSelection : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic) NSArray<NSDictionary *> *rows;
@property(nonatomic) NSTableView *table;
@property(nonatomic) BOOL importing;
- (NSView *)view;
- (NSSet<NSString *> *)keys;
@end
@implementation MDHostSelection
- (NSView *)view {
    self.table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 620, 230)];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"Review"];
    col.width = 605;
    [self.table addTableColumn:col];
    self.table.headerView = nil; self.table.allowsMultipleSelection = YES;
    self.table.rowHeight = self.importing ? 74 : 24;
    self.table.dataSource = self; self.table.delegate = self;
    NSScrollView *scroll = TableScroll(self.table, 620, 230);
    NSStackView *stack = Stack(@[scroll, Button(@"Select All", self, @selector(selectAll:))], NSUserInterfaceLayoutOrientationVertical);
    stack.frame = NSMakeRect(0, 0, 620, 266);
    [scroll.widthAnchor constraintEqualToConstant:620].active = YES;
    return stack;
}
- (void)selectAll:(id)sender { [self.table selectAll:sender]; }
- (NSSet<NSString *> *)keys {
    NSMutableSet *keys = [NSMutableSet set];
    [self.table.selectedRowIndexes enumerateIndexesUsingBlock:^(NSUInteger i, BOOL *stop) {
        (void)stop;
        NSString *key = self.rows[i][self.importing ? @"key" : @"id"];
        if (key) [keys addObject:key];
    }];
    return keys;
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { (void)table; return self.rows.count; }
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table; (void)column;
    NSDictionary *value = self.rows[row];
    if (!self.importing) return Label(value[@"label"]);
    NSString *text = [NSString stringWithFormat:@"%@%@\n%@\n%@ · Requested: %@", value[@"label"],
        [value[@"alias"] length] ? [@" — " stringByAppendingString:value[@"alias"]] : @"", value[@"npub"], value[@"fingerprint"], value[@"caps"]];
    NSTextField *label = Label(text); label.maximumNumberOfLines = 4;
    return label;
}
@end

@interface MDHostsPage () <NSTextFieldDelegate, NSTableViewDataSource, NSTableViewDelegate> {
    dispatch_queue_t _queue;
    MDHostsWorker *_worker;
    MDHostsBook *_book;
    MDHostsLaunch *_launch;
    atomic_bool _closing, _dirty, _wakeQueued;
    BOOL _busy, _launched, _started, _roleEditing;
    NSUInteger _generation, _launchGeneration;
    NSArray<NSDictionary *> *_rows;
    NSString *_lastOwner, *_savedBootstrap;
    NSArray<NSString *> *_lastOwnerHosts;
    NSTextField *_address, *_route, *_relays, *_search, *_directory;
    NSTextField *_status, *_error, *_progress, *_diagnostic, *_rotation, *_detail;
    NSPopUpButton *_tags, *_issues;
    NSTableView *_table;
    NSButton *_refresh, *_review, *_saved, *_edit, *_quick, *_import, *_export, *_issue, *_control, *_copyKey;
    MdLaunchMessage _phase;
}
- (BOOL)closing;
@end

static bool IsClosing(void *context) { return [(__bridge MDHostsPage *)context closing]; }

@implementation MDHostsPage
- (BOOL)closing { return atomic_load(&_closing); }
- (void)loadView {
    atomic_init(&_closing, false); atomic_init(&_dirty, false); atomic_init(&_wakeQueued, false);
    _queue = dispatch_queue_create("metadesk.macos-manager.hosts", DISPATCH_QUEUE_SERIAL);
    __weak MDHostsPage *weakSelf = self;
    _worker = [[MDHostsWorker alloc] initWithChange:^{ [weakSelf signalChanged]; }];
    _address = Field(@"Host npub, nprofile, public key or NIP-05 name", nil); _address.delegate = self;
    _route = Field(@"Optional IPv4 or IPv6 route for the host key", nil); _route.delegate = self;
    _relays = Field(@"Bootstrap relays (space-separated; ws:// is unencrypted)", nil);
    _directory = Field(@"Private state directory (empty uses default)", nil);
    _search = Field(@"Filter saved hosts", nil); _search.delegate = self;
    _status = Label(@"Opening the address book…"); _error = Label(nil); _progress = Label(nil);
    _diagnostic = Label(nil); _rotation = Label(nil); _detail = Label(nil);
    _tags = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_tags addItemWithTitle:@"All Tags"]; _tags.target = self; _tags.action = @selector(filter:);
    [_tags setAccessibilityLabel:@"Filter by tag"];
    _issues = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _issues.target = self; _issues.action = @selector(issueSelection:);
    [_issues setAccessibilityLabel:@"Address book issues"];
    _refresh = Button(@"Save Relays and Refresh Address Book", self, @selector(refresh:));
    _review = Button(@"Review and Connect…", self, @selector(reviewEntry:));
    _saved = Button(@"Review Saved Host…", self, @selector(reviewSaved:));
    _edit = Button(@"Edit Saved Host…", self, @selector(edit:));
    _copyKey = Button(@"Copy Public Key", self, @selector(copyPublicKey:));
    _quick = [NSButton checkboxWithTitle:@"Quick Connect (favorites, then recent attempts)" target:self action:@selector(filter:)];
    _import = Button(@"Import…", self, @selector(importHosts:));
    _export = Button(@"Export…", self, @selector(exportHosts:));
    _issue = Button(@"Review Issue…", self, @selector(handleIssue:));
    _control = Button(@"Cancel Connection", self, @selector(control:)); _control.hidden = YES;
    _address.target = self; _address.action = @selector(reviewEntry:);
    _table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 600, 170)];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"Host"];
    col.width = 600; [_table addTableColumn:col];
    _table.headerView = nil; _table.rowHeight = 26; _table.dataSource = self; _table.delegate = self;
    _table.target = self; _table.doubleAction = @selector(reviewSaved:);
    [_table setAccessibilityLabel:@"Saved hosts"];
    NSScrollView *list = TableScroll(_table, 600, 170);
    NSStackView *form = Stack(@[], NSUserInterfaceLayoutOrientationVertical);
    form.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
    for (NSView *v in @[_status, _rotation, _error, _relays, _directory]) Wide(form, v);
    [form addArrangedSubview:_refresh];
    Wide(form, _address); Wide(form, _route);
    [form addArrangedSubview:Stack(@[_review, _control], NSUserInterfaceLayoutOrientationHorizontal)];
    Wide(form, _progress); Wide(form, _diagnostic);
    Wide(form, _search);
    [form addArrangedSubview:Stack(@[_tags, _quick], NSUserInterfaceLayoutOrientationHorizontal)];
    Wide(form, list); Wide(form, _detail);
    [form addArrangedSubview:Stack(@[_saved, _edit, _copyKey], NSUserInterfaceLayoutOrientationHorizontal)];
    [form addArrangedSubview:Stack(@[_import, _export], NSUserInterfaceLayoutOrientationHorizontal)];
    [form addArrangedSubview:_issues]; [form addArrangedSubview:_issue];

    NSScrollView *scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES;
    scroll.documentView = form;
    form.translatesAutoresizingMaskIntoConstraints = NO;
    [form.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor].active = YES;
    self.view = scroll;
    [self render];
}
- (void)activate {
    if (!_started) { _started = YES; [self reloadBook:NO]; }
}
- (void)suspendForRoleEdit:(dispatch_block_t)ready {
    _roleEditing = YES; ++_generation; _book = nil; [self render];
    if (_launch && _phase < MD_LAUNCH_CONNECTED) {
        [_launch close:nil]; _launch=nil; ++_launchGeneration; _launched=NO; _control.hidden=YES;
        _progress.stringValue=@"Unfinished connection cancelled before changing the client role.";
    }
    dispatch_async(_queue, ^{ @autoreleasepool {
        [self->_worker close];
        dispatch_async(dispatch_get_main_queue(), ready);
    } });
}
- (void)resumeAfterRoleEdit {
    _roleEditing=NO;
    if (!self.closing && _started) [self reloadBook:NO];
}
- (void)close:(dispatch_block_t)drained {
    atomic_store(&_closing, true);
    MDHostsLaunch *launch = _launch;
    _launch = nil; _book = nil; _rows = nil; _lastOwner = nil; _lastOwnerHosts = nil;
    dispatch_group_t group = dispatch_group_create();
    if (launch) {
        dispatch_group_enter(group);
        [launch close:^{ dispatch_group_leave(group); }];
    }
    dispatch_group_async(group, _queue, ^{ @autoreleasepool { [self->_worker close]; } });
    dispatch_group_notify(group, dispatch_get_main_queue(), drained);
}
- (void)signalChanged {
    if (self.closing) return;
    atomic_store(&_dirty, true);
    [self scheduleSnapshot];
}
- (void)scheduleSnapshot {
    if (self.closing || atomic_exchange(&_wakeQueued, true)) return;
    dispatch_async(_queue, ^{ @autoreleasepool {
        atomic_store(&self->_dirty, false);
        MDHostsBook *book = self.closing ? nil : [self->_worker snapshot];
        dispatch_async(dispatch_get_main_queue(), ^{
            atomic_store(&self->_wakeQueued, false);
            if (self.closing || self->_roleEditing) return;
            if (book && !self->_busy) [self showBook:book];
            /* Operations take their own snapshot. If a wakeup raced with one,
             * refresh after it completes rather than dropping the notification. */
            if (self->_busy) atomic_store(&self->_dirty, true);
            else if (atomic_load(&self->_dirty)) [self scheduleSnapshot];
        });
    } });
}
- (void)run:(id (^)(MDHostsWorker *))work done:(void (^)(id))done {
    if (_busy || _roleEditing || self.closing) return;
    _busy = YES; [self render];
    dispatch_async(_queue, ^{ @autoreleasepool {
        id result = self.closing ? nil : work(self->_worker);
        MDHostsBook *book = self.closing ? nil : [self->_worker snapshot];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self.closing) return;
            self->_busy = NO;
            if (self->_roleEditing) return;
            if (book) [self showBook:book];
            if (done) done(result);
            if (atomic_load(&self->_dirty)) [self scheduleSnapshot];
        });
    } });
}
- (void)showBook:(MDHostsBook *)book {
    if (_lastOwner && book.owner && ![_lastOwner isEqual:book.owner]) {
        NSString *hosts = _lastOwnerHosts.count ? [@" Previous hosts: " stringByAppendingString:[_lastOwnerHosts componentsJoinedByString:@", "]] : @"";
        _rotation.stringValue = [@"Client key changed. Previous grants still name the old key. Re-request access and ask host admins to remove the old grant." stringByAppendingString:hosts];
        if (_launch && _phase < MD_LAUNCH_CONNECTED) { [_launch close:nil]; _launch = nil; ++_launchGeneration; _launched = NO; _control.hidden = YES; _progress.stringValue = @"Connection cancelled because the client role changed."; }
    }
    if (book.owner && !book.unavailable) {
        _lastOwner = book.owner;
        NSMutableArray *names = [NSMutableArray array];
        for (NSDictionary *row in book.rows) { if (names.count >= 12) break; [names addObject:row[@"title"]]; }
        _lastOwnerHosts = names;
    }
    _book = book;
    [self render];
}
- (void)render {
    _status.stringValue = _book.status ?: @"Opening the address book…";
    _refresh.enabled = !_roleEditing && !_busy && !self.closing;
    _relays.enabled = _refresh.enabled;
    if (_roleEditing) _status.stringValue=@"Reloading the client role…";
    _review.enabled = !_busy && !_launched && _book.canReview;
    _import.enabled = !_busy && _book.canEdit;
    _export.enabled = !_busy && _book.canExport;
    NSString *tag = _tags.selectedItem.representedObject;
    [_tags removeAllItems]; [_tags addItemWithTitle:@"All Tags"];
    for (NSDictionary *entry in _book.tags) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:entry[@"label"] action:NULL keyEquivalent:@""];
        item.representedObject = entry[@"id"]; [_tags.menu addItem:item];
        if ([tag isEqual:entry[@"id"]]) [_tags selectItem:item];
    }
    NSString *issue = _issues.selectedItem.representedObject;
    [_issues removeAllItems];
    for (NSDictionary *entry in _book.issues) {
        NSString *coord = entry[@"coordinate"];
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"%@ — %@", coord, entry[@"message"]] action:NULL keyEquivalent:@""];
        item.representedObject = coord; [_issues.menu addItem:item];
        if ([issue isEqual:coord]) [_issues selectItem:item];
    }
    _issues.hidden = _issue.hidden = !_book.issues.count;
    [self issueSelection:nil];
    [self filter:nil];
}
- (NSDictionary *)selected { NSInteger index = _table.selectedRow; return index >= 0 && (NSUInteger)index < _rows.count ? _rows[index] : nil; }
- (void)filter:(id)sender {
    (void)sender;
    NSString *selection = self.selected[@"key"];
    _rows = _quick.state == NSControlStateValueOn ? _book.quick : [_book rowsMatching:_search.stringValue tag:_tags.selectedItem.representedObject];
    [_table reloadData]; [_table deselectAll:nil];
    NSUInteger index = selection ? [[_rows valueForKey:@"key"] indexOfObject:selection] : NSNotFound;
    if (index != NSNotFound) [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:index] byExtendingSelection:NO];
    [self updateSelection];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { (void)table; return _rows.count; }
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table; (void)column;
    NSDictionary *r = _rows[row];
    NSString *title = [r[@"favorite"] boolValue] ? [@"★  " stringByAppendingString:r[@"title"]] : r[@"title"];
    NSTextField *view = Label(title); view.maximumNumberOfLines = 1;
    return view;
}
- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    (void)notification;
    [self updateSelection];
}
- (void)updateSelection {
    NSDictionary *row = self.selected;
    _saved.enabled = _review.enabled && row && [_book addressForKey:row[@"key"]];
    _edit.enabled = !_busy && _book.canEdit && row;
    _copyKey.enabled = row != nil;
    if (!row) { _detail.stringValue = @""; return; }
    NSMutableArray *lines = [NSMutableArray arrayWithObject:MDHostNpub(row[@"key"]) ?: @"Unknown public key"];
    for (NSString *field in @[@"alias", @"notes"]) if ([row[field] length]) [lines addObject:row[field]];
    NSArray *tags = row[@"tags"];
    if (tags.count) [lines addObject:[@"Tags: " stringByAppendingString:MDHostBoundText([tags componentsJoinedByString:@", "], 512)]];
    if ([row[@"lastConnected"] longLongValue] > 0) {
        NSDate *when = [NSDate dateWithTimeIntervalSince1970:[row[@"lastConnected"] doubleValue]];
        [lines addObject:[@"Last connected on this device: " stringByAppendingString:[NSDateFormatter localizedStringFromDate:when dateStyle:NSDateFormatterMediumStyle timeStyle:NSDateFormatterShortStyle]]];
    }
    _detail.stringValue = [lines componentsJoinedByString:@"\n"];
}
- (void)copyPublicKey:(id)sender {
    (void)sender;
    NSString *npub = MDHostNpub(self.selected[@"key"]);
    if (npub) { [NSPasteboard.generalPasteboard clearContents]; [NSPasteboard.generalPasteboard setString:npub forType:NSPasteboardTypeString]; }
}
- (BOOL)rejectSecret:(NSTextField *)field {
    NSTextView *editor = (NSTextView *)field.currentEditor;
    if (MDClassifyHostInput(editor ? editor.string : field.stringValue) != MDHostSecret) return NO;
    if (editor) { editor.string = @""; [editor.undoManager removeAllActions]; }
    field.stringValue = @"";
    _error.stringValue = @"Private keys are not host addresses. Use a public host identity.";
    return YES;
}
- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object == _address || note.object == _route) [self rejectSecret:note.object];
    if (note.object == _search) [self filter:nil];
}
- (void)refresh:(id)sender {
    (void)sender;
    [self reloadBook:YES];
}
- (void)reloadBook:(BOOL)refresh {
    if (_busy || _roleEditing || self.closing) return;
    ++_generation;
    NSString *edit = refresh && ![_relays.stringValue isEqual:_savedBootstrap] ? _relays.stringValue : nil;
    NSString *directory = MDHostTrim(_directory.stringValue);
    _error.stringValue = @"";
    [self run:^id(MDHostsWorker *worker) {
        NSString *configError = nil, *cacheError = nil;
        NSString *relays = MDHostsBootstrap(edit, &configError);
        if (self.closing) return nil;
        BOOL opened = [worker openDirectory:directory error:&cacheError];
        if (opened && relays && refresh && !self.closing && ![worker startRelays:relays])
            cacheError = @"Could not configure address book relays. Cached data remains available.";
        return @[relays ?: NSNull.null, configError ?: cacheError ?: @""];
    } done:^(NSArray *result) {
        if (result[0] != NSNull.null) self->_relays.stringValue = self->_savedBootstrap = result[0];
        self->_error.stringValue = result[1];
    }];
}
- (void)reviewEntry:(id)sender {
    (void)sender;
    BOOL rejected = [self rejectSecret:_address]; rejected |= [self rejectSecret:_route];
    if (rejected) return;
    NSString *address = _address.stringValue;
    MDHostInput input = MDClassifyHostInput(address);
    if (input != MDHostPublic && input != MDHostName) {
        _error.stringValue = @"Enter an npub, nprofile, lowercase public key or NIP-05 name. An IP address is only a route hint."; return;
    }
    NSString *route = MDHostTrim(_route.stringValue);
    if (!MDHostRouteValid(route)) { _error.stringValue = @"Enter an IPv4 or IPv6 route hint."; return; }
    [self resolve:address route:route];
}
- (void)reviewSaved:(id)sender {
    (void)sender;
    NSString *address = [_book addressForKey:self.selected[@"key"]];
    if (address) [self resolve:address route:@""];
}
- (void)resolve:(NSString *)address route:(NSString *)route {
    if (!_book.canReview || _launched) return;
    _error.stringValue = @"Checking host identity…";
    [self run:^id(MDHostsWorker *worker) {
        NSString *error = nil;
        MDHostReview *review = [worker resolve:address route:route cancelled:IsClosing data:(__bridge void *)self error:&error];
        return review ?: (id)error;
    } done:^(id value) {
        self->_error.stringValue = [value isKindOfClass:NSString.class] ? value : @"";
        if ([value isKindOfClass:MDHostReview.class]) [self presentReview:value];
    }];
}
- (void)presentReview:(MDHostReview *)review {
    if (!self.view.window || self.closing) return;
    BOOL mismatch = review.oldKey != nil;
    NSUInteger generation = _generation;
    NSString *nip = review.alias ? [NSString stringWithFormat:@"NIP-05: %@ (verified in this review)", MDHostBoundText(review.alias, 320)] : @"NIP-05: Not provided";
    NSString *identity = [NSString stringWithFormat:@"%@\n%@\nFingerprint: %@", review.npub, nip, review.fingerprint];
    NSString *body = mismatch ? [NSString stringWithFormat:@"%@\nSaved fingerprint: %@\n\nConnection is blocked. Verify the new fingerprint with the host before replacing the saved key.", identity, MDHostFingerprint(review.oldKey)] :
        [NSString stringWithFormat:@"%@\nRequested access: %@\n\nConfirm this fingerprint with the host before trusting the key.", identity, MDHostCapsText(review.caps)];
    NSAlert *alert = Alert(mismatch ? @"Host Key Changed" : @"Review Host Key", body);
    [alert addButtonWithTitle:mismatch ? @"Keep Old Key" : @"Cancel"];
    BOOL canSave = _book.canEdit && (!mismatch || [_book canReplaceReview:review]);
    NSTextField *confirmation = nil;
    if (mismatch) {
        if (canSave) {
            confirmation = Field(@"Type the new fingerprint exactly", nil);
            alert.accessoryView = confirmation;
            [alert addButtonWithTitle:@"Replace Key"];
        }
    } else {
        [alert addButtonWithTitle:@"Connect"];
        if (canSave) [alert addButtonWithTitle:@"Save Host"];
    }
    [alert beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (response == NSAlertFirstButtonReturn || self.closing || generation != self->_generation) return;
        if (mismatch || response == NSAlertThirdButtonReturn) {
            NSString *value = confirmation.stringValue;
            if (mismatch && ![self->_book canConfirmReview:review fingerprint:value]) {
                self->_error.stringValue = @"The saved key was kept. Replacement requires the exact new fingerprint."; return;
            }
            [self run:^id(MDHostsWorker *worker) { return [worker saveReview:review confirmation:value]; }
                done:^(NSString *message) { self->_error.stringValue = message.length ? message : mismatch ? @"Key replaced. Review it again to connect." : @"Host saved."; }];
        } else if (response == NSAlertSecondButtonReturn) [self startLaunch:review];
    }];
}
- (void)startLaunch:(MDHostReview *)review {
    if (_launched || _busy) return;
    [self run:^id(MDHostsWorker *worker) {
        NSString *error = nil;
        NSMutableData *config = [NSMutableData dataWithLength:sizeof(MdManagedLaunch)];
        return [worker launchConfig:config.mutableBytes review:review error:&error] ? config : (id)error;
    } done:^(id value) {
        if (![value isKindOfClass:NSData.class]) { self->_error.stringValue = value ?: @"Connection unavailable."; return; }
        NSData *data = value; const MdManagedLaunch *config = data.bytes;
        self->_launched = YES; self->_phase = 0;
        self->_control.title = @"Cancel Connection"; self->_control.hidden = NO;
        self->_progress.stringValue = MDHostLaunchText(0, 0); self->_diagnostic.stringValue = @"";
        NSString *owner = self->_book.owner, *key = review.key;
        __weak MDHostsPage *weakSelf = self;
        NSUInteger launchGeneration = ++self->_launchGeneration;
        self->_launch = [[MDHostsLaunch alloc] initWithUpdate:^(MdLaunchMessage message) {
            [weakSelf progress:message key:key owner:owner generation:launchGeneration];
        }];
        [self->_launch start:config manager:nil bundle:nil path:nil];
        [self render];
    }];
}
- (void)progress:(MdLaunchMessage)m key:(NSString *)key owner:(NSString *)owner generation:(NSUInteger)generation {
    if (self.closing || !_launch || generation != _launchGeneration) return;
    if (m >= MD_LAUNCH_RELAY_AUTH) {
        NSString *reason = MDHostLaunchDiagnostic(m);
        if (reason.length) _diagnostic.stringValue = [@[ _diagnostic.stringValue, reason ] componentsJoinedByString:@" "];
        return;
    }
    _progress.stringValue = MDHostLaunchText(m, _phase);
    if (m == MD_LAUNCH_CONNECTED || m == MD_LAUNCH_FAILED) {
        MdRecentOutcome outcome = MDHostLaunchOutcome(m, _phase);
        dispatch_async(_queue, ^{ @autoreleasepool {
            if (!self.closing) { [self->_worker recordKey:key owner:owner outcome:outcome]; [self signalChanged]; }
        } });
    }
    if (m == MD_LAUNCH_CONNECTED) { _control.title = @"Disconnect"; _diagnostic.stringValue = @""; }
    else if (m >= MD_LAUNCH_FAILED) {
        _control.hidden = YES; _launched = NO; [_launch close:nil]; _launch = nil;
        if (m != MD_LAUNCH_FAILED) _diagnostic.stringValue = @"";
    }
    if (m <= MD_LAUNCH_CONNECTED) _phase = m;
    [self render];
}
- (void)control:(id)sender { (void)sender; [_launch control]; }
- (void)edit:(id)sender {
    (void)sender;
    NSDictionary *row = self.selected;
    if (!_book.canEdit || _busy || !row || !self.view.window) return;
    MDHostsBook *book = _book;
    NSUInteger generation = _generation;
    NSString *key = row[@"key"];
    NSAlert *alert = Alert(@"Edit Saved Host", @"Label and notes are encrypted in the address book. Select existing tag memberships or add a new tag.");
    NSTextField *label = Field(@"Label", row[@"label"]), *notes = Field(@"Notes", row[@"notes"]), *newTag = Field(@"New tag (optional)", nil);
    NSButton *favorite = [NSButton checkboxWithTitle:@"Favorite" target:nil action:NULL];
    favorite.state = [row[@"favorite"] boolValue] ? NSControlStateValueOn : NSControlStateValueOff;
    MDHostSelection *selection = [MDHostSelection new]; selection.rows = book.tags;
    NSView *tags = [selection view];
    NSMutableIndexSet *selected = [NSMutableIndexSet indexSet];
    [book.tags enumerateObjectsUsingBlock:^(NSDictionary *value, NSUInteger index, BOOL *stop) {
        (void)stop; if ([row[@"tagIDs"] containsObject:value[@"id"]]) [selected addIndex:index];
    }];
    [selection.table selectRowIndexes:selected byExtendingSelection:NO];
    NSStackView *form = Stack(@[], NSUserInterfaceLayoutOrientationVertical);
    for (NSView *v in @[label, notes, newTag]) Wide(form, v);
    [form addArrangedSubview:favorite];
    if (book.tags.count) [form addArrangedSubview:tags];
    form.frame = NSMakeRect(0, 0, 620, book.tags.count ? 390 : 116);
    alert.accessoryView = form;
    [alert addButtonWithTitle:@"Cancel"]; [alert addButtonWithTitle:@"Save"]; [alert addButtonWithTitle:@"Remove…"];
    [alert beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (self.closing || generation != self->_generation || response == NSAlertFirstButtonReturn) return;
        void (^edit)(BOOL) = ^(BOOL remove) {
            NSString *name = label.stringValue, *memo = notes.stringValue, *tag = MDHostTrim(newTag.stringValue);
            BOOL fav = favorite.state == NSControlStateValueOn;
            NSSet *membership = [selection keys];
            [self run:^id(MDHostsWorker *worker) {
                return [worker editKey:key book:book label:name notes:memo favorite:fav tagIDs:membership newTag:tag remove:remove];
            } done:^(NSString *message) { self->_error.stringValue = message.length ? message : remove ? @"Host removed." : @"Host updated."; }];
        };
        if (response == NSAlertSecondButtonReturn) edit(NO);
        else if (response == NSAlertThirdButtonReturn && self.view.window) {
            NSAlert *remove = Alert(@"Remove Saved Host?", @"This removes the saved identity and its local address-book memberships. It does not revoke access at the host.");
            [remove addButtonWithTitle:@"Cancel"]; [remove addButtonWithTitle:@"Remove Host"];
            [remove beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse confirm) {
                if (confirm == NSAlertSecondButtonReturn && generation == self->_generation && !self.closing) edit(YES);
            }];
        }
    }];
}
- (NSDictionary *)selectedIssue {
    NSString *coordinate = _issues.selectedItem.representedObject;
    for (NSDictionary *entry in _book.issues) if ([entry[@"coordinate"] isEqual:coordinate]) return entry;
    return nil;
}
- (void)issueSelection:(id)sender {
    (void)sender;
    NSDictionary *entry = [self selectedIssue];
    BOOL conflict = [entry[@"conflict"] boolValue], retry = [entry[@"retry"] boolValue];
    _issue.title = retry ? @"Retry Pending Edit" : @"Review Conflict…";
    _issue.enabled = !_busy && [entry[@"ready"] boolValue] && (conflict || retry);
}
- (void)handleIssue:(id)sender {
    (void)sender;
    NSDictionary *entry = [self selectedIssue];
    NSString *coordinate = entry[@"coordinate"];
    MDHostsBook *book = _book;
    if (_busy || !coordinate || ![entry[@"ready"] boolValue]) return;
    if ([entry[@"retry"] boolValue]) {
        [self run:^id(MDHostsWorker *worker) { return [worker retryIssue:coordinate book:book]; }
            done:^(NSString *message) { self->_error.stringValue = message.length ? message : @"Pending edit synchronized."; }];
        return;
    }
    [self run:^id(MDHostsWorker *worker) { (void)worker; return [book conflictText:coordinate]; }
        done:^(NSString *text) {
        if (!text || !self.view.window) return;
        NSAlert *alert = Alert(@"Review Address Book Conflict", @"Review both versions. Reapply publishes your reviewed draft against the current remote revision.");
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 620, 250)];
        scroll.hasVerticalScroller = YES; scroll.borderType = NSBezelBorder;
        MDPrivateEditor *display = [[MDPrivateEditor alloc] initWithFrame:scroll.contentView.bounds];
        display.editable = NO; display.selectable = NO; display.richText = NO;
        display.horizontallyResizable = NO; display.verticallyResizable = YES;
        display.autoresizingMask = NSViewWidthSizable;
        display.textContainer.widthTracksTextView = YES;
        display.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        display.string = text;
        scroll.documentView = display; alert.accessoryView = scroll;
        [alert addButtonWithTitle:@"Cancel"]; [alert addButtonWithTitle:@"Keep Remote"]; [alert addButtonWithTitle:@"Reapply Draft"];
        [alert beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
            if (self.closing || (response != NSAlertSecondButtonReturn && response != NSAlertThirdButtonReturn)) return;
            [self run:^id(MDHostsWorker *worker) { return [worker resolveIssue:coordinate book:book reapply:response == NSAlertThirdButtonReturn]; }
                done:^(NSString *message) { self->_error.stringValue = message.length ? message : @"Conflict resolved."; }];
        }];
    }];
}
- (void)importHosts:(id)sender {
    (void)sender;
    if (_busy || !_book.canEdit || !self.view.window) return;
    MDHostsBook *book = _book;
    NSUInteger generation = _generation;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO; panel.allowsMultipleSelection = NO;
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (response != NSModalResponseOK || self.closing || generation != self->_generation) return;
        NSURL *url = panel.URL;
        [self run:^id(MDHostsWorker *worker) { return [worker readImport:url book:book]; }
            done:^(NSArray *rows) {
            if (!rows.count) { self->_error.stringValue = rows ? @"No new host keys in the file." : @"Not a supported metadesk-hosts/1 file, or the file could not be read."; return; }
            [self reviewImport:rows book:book];
        }];
    }];
}
- (void)reviewImport:(NSArray *)rows book:(MDHostsBook *)book {
    if (!self.view.window) return;
    NSAlert *alert = Alert(@"Review Imported Host Keys", @"Select only keys whose fingerprints you recognize. NIP-05 names in this file are unverified. Existing hosts will not be overwritten.");
    MDHostSelection *selection = [MDHostSelection new]; selection.importing = YES; selection.rows = rows;
    alert.accessoryView = [selection view];
    [alert addButtonWithTitle:@"Cancel"]; [alert addButtonWithTitle:@"Import Selected Hosts"];
    [alert beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (response != NSAlertSecondButtonReturn || self.closing) return;
        NSSet *keys = selection.keys;
        [self run:^id(MDHostsWorker *worker) { return [worker applyImport:rows selected:keys book:book]; }
            done:^(NSString *message) { self->_error.stringValue = message.length ? message : @"Selected hosts imported."; }];
    }];
}
- (void)exportHosts:(id)sender {
    (void)sender;
    if (_busy || !_book.canExport || !self.view.window) return;
    MDHostsBook *book = _book;
    NSUInteger generation = _generation;
    NSAlert *alert = Alert(@"Export Saved Hosts?", @"The export is plaintext. Anyone who can read the file can see the host keys, labels and notes. It does not contain credentials or access grants.");
    [alert addButtonWithTitle:@"Cancel"]; [alert addButtonWithTitle:@"Export…"];
    [alert beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (response != NSAlertSecondButtonReturn || self.closing || generation != self->_generation || !self.view.window) return;
        NSSavePanel *panel = [NSSavePanel savePanel]; panel.nameFieldStringValue = @"metadesk-hosts.json";
        [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse selected) {
            if (selected != NSModalResponseOK || self.closing || generation != self->_generation) return;
            NSURL *url = panel.URL;
            [self run:^id(MDHostsWorker *worker) { return [worker exportBook:book to:url]; }
                done:^(NSString *message) { self->_error.stringValue = message; }];
        }];
    }];
}
@end
