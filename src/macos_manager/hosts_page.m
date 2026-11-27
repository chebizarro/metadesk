#import "hosts_page.h"
#import "hosts_launch.h"
#import "host_qr.h"
#import "ui.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdatomic.h>

static NSTextField *Field(NSString *placeholder, NSString *value) {
    NSTextField *field = MDKeyField(placeholder, placeholder);
    field.stringValue = value ?: @"";
    return field;
}

/* Checkbox list for choosing tag memberships or hosts to import. Nothing is
 * chosen for the user except existing tag memberships. */
@interface MDHostChoices : NSObject
- (instancetype)initWithRows:(NSArray<NSDictionary *> *)rows importing:(BOOL)importing selected:(NSSet<NSString *> *)selected;
@property(nonatomic, readonly) NSView *view;
@property(nonatomic, copy) dispatch_block_t changed;
- (NSSet<NSString *> *)keys;
@end
@implementation MDHostChoices {
    NSArray<NSDictionary *> *_rows;
    NSMutableArray<NSButton *> *_boxes;
    BOOL _importing;
}
- (instancetype)initWithRows:(NSArray<NSDictionary *> *)rows importing:(BOOL)importing selected:(NSSet<NSString *> *)selected {
    if (!(self = [super init])) return nil;
    _rows = rows; _importing = importing; _boxes = [NSMutableArray array];
    NSStackView *list = [NSStackView stackViewWithViews:@[]];
    list.orientation = NSUserInterfaceLayoutOrientationVertical; list.alignment = NSLayoutAttributeLeading;
    list.spacing = importing ? 12 : 6; list.edgeInsets = NSEdgeInsetsMake(8, 10, 8, 10);
    list.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSDictionary *row in rows) {
        NSString *key = row[importing ? @"key" : @"id"];
        NSString *title = importing ? ([row[@"label"] length] ? row[@"label"] : @"Unnamed host") : row[@"label"];
        NSButton *box = [NSButton checkboxWithTitle:MDHostBoundText(title, 128) target:self action:@selector(toggle:)];
        box.state = [selected containsObject:key] ? NSControlStateValueOn : NSControlStateValueOff;
        [_boxes addObject:box];
        if (!importing) { [list addArrangedSubview:box]; continue; }
        NSTextField *npub = MDValue(row[@"npub"]);
        npub.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
        NSMutableArray *details = [NSMutableArray arrayWithArray:@[@[@"Fingerprint", MDKeyValue(row[@"fingerprint"])],
            @[@"Public key", npub], @[@"Requests", row[@"caps"] ?: @""]]];
        if ([row[@"alias"] length]) [details insertObject:@[@"NIP-05", [row[@"alias"] stringByAppendingString:@" (not verified)"]] atIndex:0];
        NSGridView *grid = MDDetails(details);
        for (NSInteger r = 0; r < grid.numberOfRows; ++r) {
            NSTextField *label = (NSTextField *)[grid cellAtColumnIndex:0 rowIndex:r].contentView;
            if ([label isKindOfClass:NSTextField.class]) { label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize]; label.textColor = NSColor.secondaryLabelColor; }
            NSTextField *value = (NSTextField *)[grid cellAtColumnIndex:1 rowIndex:r].contentView;
            if ([value isKindOfClass:NSTextField.class]) value.font = [value.font fontWithSize:NSFont.smallSystemFontSize];
        }
        NSView *entry = [NSView new];
        for (NSView *v in @[box, grid]) { v.translatesAutoresizingMaskIntoConstraints = NO; [entry addSubview:v]; }
        [NSLayoutConstraint activateConstraints:@[
            [box.topAnchor constraintEqualToAnchor:entry.topAnchor], [box.leadingAnchor constraintEqualToAnchor:entry.leadingAnchor],
            [grid.topAnchor constraintEqualToAnchor:box.bottomAnchor constant:4], [grid.bottomAnchor constraintEqualToAnchor:entry.bottomAnchor],
            [grid.leadingAnchor constraintEqualToAnchor:entry.leadingAnchor constant:20], [grid.trailingAnchor constraintEqualToAnchor:entry.trailingAnchor]]];
        [list addArrangedSubview:entry];
        [entry.widthAnchor constraintEqualToAnchor:list.widthAnchor constant:-20].active = YES;
    }
    NSScrollView *scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES; scroll.borderType = NSBezelBorder;
    NSView *document = [NSView new];
    document.translatesAutoresizingMaskIntoConstraints = NO;
    [document addSubview:list];
    scroll.documentView = document;
    [NSLayoutConstraint activateConstraints:@[
        [list.topAnchor constraintEqualToAnchor:document.topAnchor], [list.bottomAnchor constraintEqualToAnchor:document.bottomAnchor],
        [list.leadingAnchor constraintEqualToAnchor:document.leadingAnchor], [list.trailingAnchor constraintEqualToAnchor:document.trailingAnchor],
        [document.topAnchor constraintEqualToAnchor:scroll.contentView.topAnchor],
        [document.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor]]];
    NSLayoutConstraint *fit = [scroll.heightAnchor constraintEqualToAnchor:document.heightAnchor constant:2];
    fit.priority = NSLayoutPriorityDefaultLow; fit.active = YES;
    [scroll.heightAnchor constraintLessThanOrEqualToConstant:importing ? 260 : 150].active = YES;
    [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:40].active = YES;
    if (!importing) { _view = scroll; return self; }
    NSButton *all = MDButton(@"Select All", self, @selector(selectAll:));
    NSButton *none = MDButton(@"Select None", self, @selector(selectNone:));
    NSStackView *buttons = MDRow(@[all, none]);
    _view = MDColumn(@[scroll, buttons], MDItemSpacing);
    return self;
}
- (void)toggle:(id)sender { (void)sender; if (self.changed) self.changed(); }
- (void)selectAll:(id)sender { for (NSButton *b in _boxes) b.state = NSControlStateValueOn; [self toggle:sender]; }
- (void)selectNone:(id)sender { for (NSButton *b in _boxes) b.state = NSControlStateValueOff; [self toggle:sender]; }
- (NSSet<NSString *> *)keys {
    NSMutableSet *keys = [NSMutableSet set];
    for (NSUInteger i = 0; i < _boxes.count; ++i) if (_boxes[i].state == NSControlStateValueOn) {
        NSString *key = _rows[i][_importing ? @"key" : @"id"];
        if (key) [keys addObject:key];
    }
    return keys;
}
@end

@interface MDHostTable : MDTable
@end
@implementation MDHostTable
/* Return opens the selected host's review, like a double-click. */
- (void)keyDown:(NSEvent *)event {
    if ([event.charactersIgnoringModifiers isEqual:@"\r"] && self.selectedRow >= 0 && self.doubleAction) {
        [NSApp sendAction:self.doubleAction to:self.target from:self];
        return;
    }
    [super keyDown:event];
}
@end

@interface MDHostsPage () <NSTextFieldDelegate, NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate> {
    dispatch_queue_t _queue;
    MDHostsWorker *_worker;
    MDHostsBook *_book;
    MDHostsLaunch *_launch;
    atomic_bool _closing, _dirty, _wakeQueued;
    BOOL _busy, _launched, _started, _roleEditing, _syncPaused;
    NSUInteger _generation, _launchGeneration;
    NSArray<NSDictionary *> *_rows;
    NSString *_lastOwner, *_savedBootstrap, *_relayDraft, *_directoryDraft, *_query, *_diagnosticText;
    NSArray<NSString *> *_lastOwnerHosts;
    NSTextField *_address, *_route;
    MDStatusView *_message, *_connection, *_rotation, *_sync, *_issueStatus;
    NSTextField *_detailKey, *_detailFingerprint, *_detailNotes, *_emptyTitle, *_emptyDetail;
    NSGridView *_details;
    NSView *_empty;
    NSSegmentedControl *_scope;
    NSPopUpButton *_tags, *_issues, *_actions;
    NSStackView *_connectionRow, *_issueRow;
    NSTableView *_table;
    NSButton *_review, *_scan, *_saved, *_edit, *_copyKey, *_control, *_issue, *_relaysButton;
    MdLaunchMessage _phase;
}
- (BOOL)closing;
@end

static bool IsClosing(void *context) { return [(__bridge MDHostsPage *)context closing]; }

@implementation MDHostsPage
- (instancetype)initWithWorker:(MDHostsWorker *)worker {
    if ((self=[super initWithNibName:nil bundle:nil])) _worker=worker;
    return self;
}
- (BOOL)closing { return atomic_load(&_closing); }
- (NSTableView *)hostTable {
    MDHostTable *table = [MDHostTable new];
    NSTableColumn *star = MDColumnNamed(table, @"Favorite", @"", 22, nil);
    star.resizingMask = NSTableColumnNoResizing; star.minWidth = star.maxWidth = 22;
    star.headerToolTip = @"Favorite";
    star.sortDescriptorPrototype = [NSSortDescriptor sortDescriptorWithKey:@"favorite" ascending:NO];
    MDColumnNamed(table, @"Name", @"Name", 180, @"title").minWidth = 120;
    MDColumnNamed(table, @"Alias", @"NIP-05", 140, @"alias");
    MDColumnNamed(table, @"Tags", @"Tags", 100, @"tagText");
    NSTableColumn *last = MDColumnNamed(table, @"Last", @"Last Connected", 120, nil);
    last.sortDescriptorPrototype = [NSSortDescriptor sortDescriptorWithKey:@"lastConnected" ascending:NO];
    MDFinishColumns(table, @"Name");
    table.dataSource = self; table.delegate = self;
    table.target = self; table.doubleAction = @selector(reviewSaved:);
    table.accessibilityLabel = @"Saved hosts";
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Host"];
    [menu addItemWithTitle:@"Connect…" action:@selector(reviewSaved:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"Edit…" action:@selector(edit:) keyEquivalent:@""].target = self;
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItemWithTitle:@"Copy Public Key" action:@selector(copyPublicKey:) keyEquivalent:@""].target = self;
    menu.delegate = self;
    table.menu = menu;
    return table;
}
- (void)loadView {
    atomic_init(&_closing, false); atomic_init(&_dirty, false); atomic_init(&_wakeQueued, false);
    _queue = dispatch_queue_create("metadesk.macos-manager.hosts", DISPATCH_QUEUE_SERIAL);
    __weak MDHostsPage *weakSelf = self;
    if (!_worker) _worker = [[MDHostsWorker alloc] initWithChange:^{ [weakSelf signalChanged]; }];
    _relayDraft = @""; _directoryDraft = @""; _query = @"";

    /* Connect: an aligned form with its actions in a trailing column. */
    _address = MDKeyField(@"Host npub, nprofile, public key or NIP-05 name", @"Host");
    _address.delegate = self; _address.target = self; _address.action = @selector(reviewEntry:);
    _route = MDKeyField(@"Optional IPv4 or IPv6 address", @"Route hint");
    _route.delegate = self;
    _review = MDButton(@"Review and Connect…", self, @selector(reviewEntry:));
    _scan = MDButton(@"Scan QR Code…", self, @selector(scanQR:));
    NSGridView *connect = [NSGridView gridViewWithViews:@[@[MDFormLabel(@"Host"), _address, _review],
                                                          @[MDFormLabel(@"Route hint"), _route, _scan]]];
    connect.columnSpacing = MDItemSpacing; connect.rowSpacing = MDRowSpacing;
    connect.rowAlignment = NSGridRowAlignmentFirstBaseline;
    [connect columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [connect columnAtIndex:2].xPlacement = NSGridCellPlacementFill;
    _message = [MDStatusView new];
    _connection = [MDStatusView new];
    _control = MDButton(@"Cancel Connection", self, @selector(control:));
    _connectionRow = MDRow(@[_connection, _control]); _connectionRow.alignment = NSLayoutAttributeCenterY;
    _connectionRow.hidden = YES;
    _rotation = [MDStatusView new];

    /* Saved hosts: header with scope, tag filter and an action menu. */
    _scope = [NSSegmentedControl segmentedControlWithLabels:@[@"All Hosts", @"Quick Connect"]
        trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(filter:)];
    _scope.selectedSegment = 0; _scope.accessibilityLabel = @"Show";
    [_scope setToolTip:@"Favorites first, then hosts you tried recently on this Mac" forSegment:1];
    _tags = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_tags addItemWithTitle:@"All Tags"]; _tags.target = self; _tags.action = @selector(filter:);
    _tags.accessibilityLabel = @"Filter by tag";
    _actions = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    _actions.bezelStyle = NSBezelStyleRegularSquare; _actions.bordered = YES;
    [_actions addItemWithTitle:@""];
    _actions.itemArray[0].image = [NSImage imageWithSystemSymbolName:@"ellipsis.circle" accessibilityDescription:@"More Actions"];
    for (NSArray *a in @[@[@"Import Hosts…", NSStringFromSelector(@selector(importHosts:))],
                         @[@"Export Hosts…", NSStringFromSelector(@selector(exportHosts:))], @[@"-", @""],
                         @[@"Address Book Relays…", NSStringFromSelector(@selector(editRelays:))]]) {
        if ([a[0] isEqual:@"-"]) { [_actions.menu addItem:NSMenuItem.separatorItem]; continue; }
        NSMenuItem *item = [_actions.menu addItemWithTitle:a[0] action:NSSelectorFromString(a[1]) keyEquivalent:@""];
        item.target = self;
    }
    _actions.accessibilityLabel = @"More actions"; _actions.toolTip = @"Import, export and relay settings";
    NSStackView *header = MDRow(@[MDHeader(@"Saved Hosts")]);
    header.alignment = NSLayoutAttributeCenterY;
    for (NSView *v in @[_scope, _tags, _actions]) [header addView:v inGravity:NSStackViewGravityTrailing];
    _sync = [MDStatusView new];
    _relaysButton = MDButton(@"Relays…", self, @selector(editRelays:));
    _relaysButton.controlSize = NSControlSizeSmall; _relaysButton.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    NSStackView *syncRow = MDRow(@[_sync]); syncRow.alignment = NSLayoutAttributeCenterY;
    [syncRow addView:_relaysButton inGravity:NSStackViewGravityTrailing];
    [_sync setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _issueStatus = [MDStatusView new];
    _issues = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _issues.target = self; _issues.action = @selector(issueSelection:);
    _issues.accessibilityLabel = @"Address book issues";
    [_issues setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _issue = MDButton(@"Review Conflict…", self, @selector(handleIssue:));
    _issueRow = MDRow(@[_issueStatus, _issues, _issue]); _issueRow.alignment = NSLayoutAttributeCenterY;
    _issueRow.hidden = YES;

    _table = [self hostTable];
    NSTextField *emptyTitle = nil, *emptyDetail = nil; NSView *empty = nil;
    NSView *list = MDTableArea(MDTableScroll(_table), &empty, &emptyTitle, &emptyDetail);
    _empty = empty; _emptyTitle = emptyTitle; _emptyDetail = emptyDetail;

    _detailKey = MDValue(@""); _detailKey.font = [NSFont monospacedSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
    _detailFingerprint = MDValue(@""); _detailFingerprint.font = _detailKey.font;
    _detailNotes = MDValue(@""); _detailNotes.lineBreakMode = NSLineBreakByTruncatingTail;
    _detailKey.accessibilityLabel = @"Public key"; _detailFingerprint.accessibilityLabel = @"Fingerprint"; _detailNotes.accessibilityLabel = @"Notes";
    _details = MDForm(@[@[@"Public key", _detailKey], @[@"Fingerprint", _detailFingerprint], @[@"Notes", _detailNotes]]);
    _details.rowSpacing = 4;
    _saved = MDButton(@"Connect…", self, @selector(reviewSaved:));
    _edit = MDButton(@"Edit…", self, @selector(edit:));
    _copyKey = MDButton(@"Copy Public Key", self, @selector(copyPublicKey:));
    _saved.toolTip = @"Review the selected host, then connect";
    NSStackView *hostActions = MDRow(@[_copyKey, _edit]); hostActions.alignment = NSLayoutAttributeCenterY;
    [hostActions addView:_saved inGravity:NSStackViewGravityTrailing];

    NSStackView *stack = nil;
    NSView *page = MDPage(@[connect, _message, _connectionRow, _rotation, MDSeparator(), header, syncRow, _issueRow,
                            list, _details, hostActions], &stack);
    [stack setCustomSpacing:MDGroupSpacing afterView:_rotation];
    [stack setCustomSpacing:MDGroupSpacing afterView:stack.arrangedSubviews[4]];
    [stack setCustomSpacing:6 afterView:header];
    [stack setCustomSpacing:12 afterView:list];
    stack.detachesHiddenViews = YES;
    self.view = page;
    [self render];
}
- (void)activate {
    if (!_started) { _started = YES; [self reloadBook:NO]; }
}
- (void)suspendForRoleEdit:(dispatch_block_t)ready {
    _roleEditing = YES; ++_generation; _book = nil; [self render];
    if (_launch && _phase < MD_LAUNCH_CONNECTED) {
        [_launch close:nil]; _launch=nil; ++_launchGeneration; _launched=NO;
        [self showConnection:@"Unfinished connection cancelled before changing the client role." kind:MDStatusInfo];
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
        [_rotation setText:[@"Your client key changed. Hosts that granted access to the old key don’t know the new one: request access again, and ask each host’s admin to remove the old grant." stringByAppendingString:hosts] kind:MDStatusWarning];
        if (_launch && _phase < MD_LAUNCH_CONNECTED) {
            [_launch close:nil]; _launch = nil; ++_launchGeneration; _launched = NO;
            [self showConnection:@"Connection cancelled because the client role changed." kind:MDStatusInfo];
        }
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
/* Feedback for the last action: what happened, or what to do next. */
- (void)note:(NSString *)text kind:(MDStatusKind)kind { [_message setText:text kind:text.length ? kind : MDStatusNone]; }
- (void)noteResult:(NSString *)error success:(NSString *)success {
    if (error.length) [self note:error kind:MDStatusError]; else [self note:success kind:MDStatusOK];
}
- (void)showConnection:(NSString *)text kind:(MDStatusKind)kind {
    NSString *line = _diagnosticText.length && kind != MDStatusOK ? [NSString stringWithFormat:@"%@ %@", text ?: @"", _diagnosticText] : text;
    [_connection setText:line kind:kind];
    _connectionRow.hidden = !text.length;
    _control.hidden = !_launched;
}
- (MDStatusKind)syncKind {
    if (_roleEditing) return MDStatusWorking;
    if (!_book || _book.unavailable) return _book ? MDStatusError : MDStatusWorking;
    NSString *status = _book.status;
    if ([status isEqual:@"Address book synced."]) return _syncPaused && _savedBootstrap.length ? MDStatusWarning : MDStatusOK;
    if ([status hasSuffix:@"…"]) return MDStatusWorking;
    return MDStatusWarning;
}
- (void)render {
    NSString *status = _book.status ?: @"Opening the address book…";
    if (_syncPaused && _savedBootstrap.length)
        status = [status stringByAppendingString:@" Relay sync is paused. Choose View > Refresh to resume."];
    if (_roleEditing) status = @"Reloading the client role…";
    [_sync setText:status kind:[self syncKind]];
    _relaysButton.enabled = !_roleEditing && !_busy && !self.closing;
    _review.enabled = !_busy && !_launched && _book.canReview;
    _scan.enabled = _review.enabled;
    _address.enabled = _route.enabled = !_roleEditing && !self.closing;
    NSString *tag = _tags.selectedItem.representedObject;
    [_tags removeAllItems]; [_tags addItemWithTitle:@"All Tags"];
    if (_book.tags.count) [_tags.menu addItem:NSMenuItem.separatorItem];
    for (NSDictionary *entry in _book.tags) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:entry[@"label"] action:NULL keyEquivalent:@""];
        item.representedObject = entry[@"id"]; [_tags.menu addItem:item];
        if ([tag isEqual:entry[@"id"]]) [_tags selectItem:item];
    }
    _tags.enabled = _book.tags.count != 0 && _scope.selectedSegment == 0;
    NSString *issue = _issues.selectedItem.representedObject;
    [_issues removeAllItems];
    for (NSDictionary *entry in _book.issues) {
        NSString *coord = entry[@"coordinate"];
        NSString *collection = [coord componentsSeparatedByString:@":"].lastObject ?: @"";
        NSString *title = [NSString stringWithFormat:@"%@ (%@)", MDHostBoundText(entry[@"message"], 160), MDHostBoundText(collection, 64)];
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""];
        item.representedObject = coord; [_issues.menu addItem:item];
        if ([issue isEqual:coord]) [_issues selectItem:item];
    }
    NSUInteger issues = _book.issues.count;
    _issueRow.hidden = !issues;
    [_issueStatus setText:issues == 1 ? @"1 sync issue:" : [NSString stringWithFormat:@"%lu sync issues:", (unsigned long)issues] kind:MDStatusWarning];
    [self issueSelection:nil];
    [self filter:nil];
}
- (NSDictionary *)selected { NSInteger index = _table.selectedRow; return index >= 0 && (NSUInteger)index < _rows.count ? _rows[index] : nil; }
- (NSArray<NSDictionary *> *)sorted:(NSArray<NSDictionary *> *)rows {
    NSMutableArray *out = [NSMutableArray arrayWithCapacity:rows.count];
    for (NSDictionary *row in rows) {
        NSMutableDictionary *r = [row mutableCopy];
        r[@"tagText"] = [row[@"tags"] componentsJoinedByString:@", "] ?: @"";
        [out addObject:r];
    }
    /* Quick Connect keeps the model's order: favorites, then recent attempts. */
    if (_scope.selectedSegment == 0 && _table.sortDescriptors.count) [out sortUsingDescriptors:_table.sortDescriptors];
    return out;
}
- (void)filter:(id)sender {
    (void)sender;
    NSString *selection = self.selected[@"key"];
    BOOL quick = _scope.selectedSegment == 1;
    _tags.enabled = !quick && _book.tags.count != 0;
    _rows = [self sorted:quick ? _book.quick : [_book rowsMatching:_query ?: @"" tag:_tags.selectedItem.representedObject]];
    [_table reloadData]; [_table deselectAll:nil];
    NSUInteger index = selection ? [[_rows valueForKey:@"key"] indexOfObject:selection] : NSNotFound;
    if (index != NSNotFound) [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:index] byExtendingSelection:NO];
    [self updateEmptyState];
    [self updateSelection];
}
- (void)updateEmptyState {
    BOOL filtered = _query.length || _tags.selectedItem.representedObject;
    _empty.hidden = _rows.count != 0;
    if (_rows.count) return;
    if (!_book) { _emptyTitle.stringValue = @"Opening Address Book…"; _emptyDetail.stringValue = @""; }
    else if (_book.unavailable) { _emptyTitle.stringValue = @"Address Book Unavailable"; _emptyDetail.stringValue = @"See the message above for what to do next."; }
    else if (_scope.selectedSegment == 1) { _emptyTitle.stringValue = @"No Favorites or Recent Hosts"; _emptyDetail.stringValue = @"Mark a host as a favorite in Edit, or connect to one, and it appears here."; }
    else if (filtered) { _emptyTitle.stringValue = @"No Matching Hosts"; _emptyDetail.stringValue = @"Try a different search or tag."; }
    else { _emptyTitle.stringValue = @"No Saved Hosts"; _emptyDetail.stringValue = @"Enter a host above and choose Save Host in its review, or import a hosts file."; }
}
- (void)tableView:(NSTableView *)table sortDescriptorsDidChange:(NSArray<NSSortDescriptor *> *)old {
    (void)table; (void)old;
    if (_scope.selectedSegment == 1 && _table.sortDescriptors.count) _scope.selectedSegment = 0;
    [self filter:nil];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { (void)table; return (NSInteger)_rows.count; }
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    NSDictionary *r = _rows[(NSUInteger)row];
    NSString *identifier = column.identifier;
    if ([identifier isEqual:@"Favorite"]) {
        NSTableCellView *cell = [table makeViewWithIdentifier:@"FavoriteCell" owner:nil];
        if (!cell) {
            cell = [NSTableCellView new]; cell.identifier = @"FavoriteCell";
            NSImageView *image = [NSImageView new];
            image.translatesAutoresizingMaskIntoConstraints = NO; image.contentTintColor = NSColor.systemYellowColor;
            [cell addSubview:image]; cell.imageView = image;
            [image.centerXAnchor constraintEqualToAnchor:cell.centerXAnchor].active = YES;
            [image.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor].active = YES;
        }
        BOOL favorite = [r[@"favorite"] boolValue];
        cell.imageView.image = favorite ? [NSImage imageWithSystemSymbolName:@"star.fill" accessibilityDescription:@"Favorite"] : nil;
        cell.accessibilityLabel = favorite ? @"Favorite" : @"Not a favorite";
        return cell;
    }
    if ([identifier isEqual:@"Name"]) return MDCell(table, @"NameCell", r[@"title"]);
    if ([identifier isEqual:@"Alias"]) return MDCell(table, @"AliasCell", [r[@"alias"] length] ? r[@"alias"] : @"—");
    if ([identifier isEqual:@"Tags"]) return MDCell(table, @"TagsCell", MDHostBoundText(r[@"tagText"], 512));
    NSString *when = @"Never";
    if ([r[@"lastConnected"] longLongValue] > 0) {
        NSDate *date = [NSDate dateWithTimeIntervalSince1970:[r[@"lastConnected"] doubleValue]];
        when = [NSDateFormatter localizedStringFromDate:date dateStyle:NSDateFormatterShortStyle timeStyle:NSDateFormatterShortStyle];
    }
    NSTableCellView *cell = MDCell(table, @"LastCell", when);
    cell.textField.textColor = [r[@"lastConnected"] longLongValue] > 0 ? NSColor.labelColor : NSColor.secondaryLabelColor;
    return cell;
}
- (NSString *)tableView:(NSTableView *)table typeSelectStringForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table; (void)column;
    return _rows[(NSUInteger)row][@"title"];
}
- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    (void)notification;
    [self updateSelection];
}
- (void)menuNeedsUpdate:(NSMenu *)menu {
    (void)menu;
    NSInteger clicked = _table.clickedRow;
    if (clicked >= 0 && ![_table isRowSelected:clicked])
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)clicked] byExtendingSelection:NO];
}
- (void)updateSelection {
    NSDictionary *row = self.selected;
    _saved.enabled = _review.enabled && row && [_book addressForKey:row[@"key"]];
    _edit.enabled = !_busy && _book.canEdit && row;
    _copyKey.enabled = row != nil;
    _details.hidden = !row;
    if (!row) return;
    _detailKey.stringValue = MDHostNpub(row[@"key"]) ?: @"Unknown public key";
    _detailKey.toolTip = _detailKey.stringValue;
    _detailFingerprint.stringValue = MDHostFingerprint(row[@"key"]) ?: @"";
    _detailNotes.stringValue = [row[@"notes"] length] ? row[@"notes"] : @"None";
    _detailNotes.toolTip = [row[@"notes"] length] ? row[@"notes"] : nil;
    _detailNotes.textColor = [row[@"notes"] length] ? NSColor.labelColor : NSColor.secondaryLabelColor;
}
/* Copy Public Key and Edit > Copy with a host selected put that host's npub
 * on the pasteboard; only ever in response to the user's command. */
- (void)copyPublicKey:(id)sender {
    (void)sender;
    NSString *npub = MDHostNpub(self.selected[@"key"]);
    if (npub) { [NSPasteboard.generalPasteboard clearContents]; [NSPasteboard.generalPasteboard setString:npub forType:NSPasteboardTypeString]; }
}
- (void)copy:(id)sender { [self copyPublicKey:sender]; }
/* A secret key is cleared without being echoed, and the undo stack that held
 * the typed or pasted text is discarded so Undo cannot bring it back. */
- (BOOL)rejectSecret:(NSTextField *)field input:(MDHostInput)input {
    if (input != MDHostSecret) return NO;
    NSTextView *editor = (NSTextView *)field.currentEditor;
    if (editor) {
        NSUndoManager *undo = editor.undoManager;
        editor.string = @"";
        [undo removeAllActions];
    }
    [field.window.undoManager removeAllActions];
    field.stringValue = @"";
    [self note:@"Private keys are not host addresses, so the field was cleared. Enter the host’s public npub, nprofile or NIP-05 name." kind:MDStatusError];
    return YES;
}
- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object == _address || note.object == _route) {
        NSTextField *field = note.object;
        NSTextView *editor = (NSTextView *)field.currentEditor;
        [self rejectSecret:field input:MDClassifyHostInput(editor ? editor.string : field.stringValue)];
    }
}
- (void)refresh:(id)sender {
    (void)sender;
    [self reloadBook:YES];
}
- (void)focusHostEntry:(id)sender {
    (void)sender;
    if (_address.enabled) [self.view.window makeFirstResponder:_address];
}
- (void)focusSearch:(id)sender { (void)sender; }
- (void)setSearchText:(NSString *)text {
    _query = text ?: @"";
    [self filter:nil];
}
- (BOOL)validateMenuItem:(NSMenuItem *)item { return [self validateUserInterfaceItem:item]; }
- (BOOL)validateToolbarItem:(NSToolbarItem *)item { return [self validateUserInterfaceItem:item]; }
- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item {
    SEL action = item.action;
    BOOL idle = !_busy && !_roleEditing && !self.closing;
    if (action == @selector(refresh:) || action == @selector(editRelays:)) return idle;
    if (action == @selector(focusHostEntry:) || action == @selector(focusSearch:)) return !self.closing;
    if (action == @selector(scanQR:)) return idle && !_launched && _book.canReview;
    if (action == @selector(importHosts:)) return idle && _book.canEdit;
    if (action == @selector(exportHosts:)) return idle && _book.canExport;
    if (action == @selector(copy:) || action == @selector(copyPublicKey:)) return self.selected != nil;
    if (action == @selector(edit:)) return idle && _book.canEdit && self.selected;
    if (action == @selector(reviewSaved:)) return _saved.enabled;
    return [self respondsToSelector:action];
}
- (void)editRelays:(id)sender {
    (void)sender;
    NSWindow *window = self.view.window;
    if (_busy || _roleEditing || self.closing || !window || window.attachedSheet) return;
    MDSheet *sheet = [[MDSheet alloc] initWithTitle:@"Address Book Relays"
        message:@"Metadesk syncs your encrypted address book through these bootstrap relays. Separate several with spaces. "
                @"Nothing is used unless you list it here; ws:// addresses are unencrypted." width:520];
    NSTextField *relays = MDKeyField(@"wss://relay.example.com", @"Bootstrap relays");
    relays.stringValue = _relayDraft ?: @"";
    NSTextField *directory = MDKeyField(@"Default", @"Private state folder");
    directory.stringValue = _directoryDraft ?: @"";
    NSTextField *hint = MDNote(@"Leave the state folder empty unless you keep a separate private cache. International hostnames must be entered in their xn-- form.");
    sheet.accessory = MDForm(@[@[@"Relays", relays], @[@"State folder", directory], @[NSNull.null, hint]]);
    sheet.initialFirstResponder = relays;
    [sheet addButton:@"Save and Sync" role:MDSheetDefault response:NSAlertFirstButtonReturn];
    [sheet addButton:@"Cancel" role:MDSheetCancel response:NSAlertSecondButtonReturn];
    [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (response != NSAlertFirstButtonReturn || self.closing) return;
        self->_relayDraft = relays.stringValue; self->_directoryDraft = directory.stringValue;
        [self reloadBook:YES];
    }];
}
- (void)reloadBook:(BOOL)refresh {
    [self reloadBook:refresh replaceUnreadable:NO];
}
- (void)reloadBook:(BOOL)refresh replaceUnreadable:(BOOL)replace {
    if (_busy || _roleEditing || self.closing) return;
    NSUInteger generation = ++_generation;
    NSString *draft = _relayDraft ?: @"";
    BOOL edited = ![draft isEqual:_savedBootstrap ?: @""];
    NSString *directory = MDHostTrim(_directoryDraft ?: @"");
    [self note:nil kind:MDStatusNone];
    [self run:^id(MDHostsWorker *worker) {
        NSString *configError = nil, *cacheError = nil;
        NSString *saved = MDHostsBootstrap(nil, &configError);
        if (self.closing) return nil;
        BOOL needsConfirmation = refresh && !saved && !replace;
        NSString *relays = saved;
        if (refresh && ((saved && edited) || (!saved && replace))) {
            configError = nil;
            relays = MDHostsBootstrap(draft, &configError);
        }
        if (self.closing) return nil;
        BOOL opened = [worker openDirectory:directory error:&cacheError];
        BOOL syncing = opened && relays && refresh && !self.closing && [worker startRelays:relays];
        if (opened && relays && refresh && !syncing)
            cacheError = @"Could not configure address book relays. Cached data remains available.";
        return @[relays ?: NSNull.null, configError ?: cacheError ?: @"", @(needsConfirmation), @(!syncing)];
    } done:^(NSArray *result) {
        if (result[0] != NSNull.null) self->_relayDraft = self->_savedBootstrap = result[0];
        self->_syncPaused = [result[3] boolValue];
        [self note:result[1] kind:MDStatusError];
        [self render];
        NSWindow *window = self.view.window;
        if (![result[2] boolValue] || !window || window.attachedSheet) return;
        MDSheet *sheet = [[MDSheet alloc] initWithTitle:@"Replace Unreadable Relay Settings?"
            message:@"Metadesk couldn’t read the saved relay settings file. Replace it with the relays in Address Book Relays? If that list is empty, every saved relay is removed. "
                    @"The Linux Manager reads the same file."
              width:500];
        [sheet addButton:@"Replace Settings" role:MDSheetDestructive response:NSAlertSecondButtonReturn];
        [sheet addButton:@"Keep Existing File" role:MDSheetCancel response:NSAlertFirstButtonReturn];
        [sheet makeCancelAlsoDefault];
        [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
            if (response == NSAlertSecondButtonReturn && generation == self->_generation)
                [self reloadBook:YES replaceUnreadable:YES];
        }];
    }];
}
- (void)reviewEntry:(id)sender {
    (void)sender;
    [self reviewInput:_address.stringValue fromImage:NO];
}
- (void)reviewInput:(NSString *)address fromImage:(BOOL)fromImage {
    MDHostInput input = MDClassifyHostInput(address);
    BOOL rejected = [self rejectSecret:_address input:input];
    rejected |= [self rejectSecret:_route input:MDClassifyHostInput(_route.stringValue)];
    if (rejected) return;
    if (fromImage) {
        address = MDHostQRProfile(address);
        if (!address) { [self note:@"That QR code doesn’t contain a host’s public nprofile, so nothing was reviewed." kind:MDStatusError]; return; }
        input = MDHostPublic;
    }
    if (input != MDHostPublic && input != MDHostName) {
        [self note:@"Enter the host’s npub, nprofile, lowercase hex public key or NIP-05 name. An IP address goes in Route hint." kind:MDStatusError]; return;
    }
    NSString *route = MDHostTrim(_route.stringValue);
    if (!MDHostRouteValid(route)) { [self note:@"The route hint must be an IPv4 or IPv6 address, or empty." kind:MDStatusError]; return; }
    if (fromImage) _address.stringValue = MDHostTrim(address);
    [self resolve:address route:route];
}
- (void)scanQR:(id)sender {
    (void)sender;
    NSWindow *window = self.view.window;
    if (!_scan.enabled || _busy || self.closing || !window || window.attachedSheet) return;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedContentTypes = @[UTTypeImage];
    panel.message = @"Choose an image of the host’s QR code. Only a public nprofile is accepted.";
    panel.prompt = @"Scan";
    NSUInteger generation = _generation;
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (response != NSModalResponseOK || self.closing || generation != self->_generation) return;
        NSURL *url = panel.URL;
        [self run:^id(MDHostsWorker *worker) { (void)worker; return MDReadHostQR(url); }
             done:^(NSString *value) {
            if (generation != self->_generation) return;
            if (!value.length) [self note:@"No single QR code was found in that image. Choose an image that contains exactly one QR code." kind:MDStatusError];
            else [self reviewInput:value fromImage:YES];
        }];
    }];
}
- (void)reviewSaved:(id)sender {
    (void)sender;
    NSString *address = [_book addressForKey:self.selected[@"key"]];
    if (address) [self resolve:address route:@""];
}
- (void)resolve:(NSString *)address route:(NSString *)route {
    if (!_book.canReview || _launched) return;
    [self note:@"Checking the host’s identity…" kind:MDStatusWorking];
    [self run:^id(MDHostsWorker *worker) {
        NSString *error = nil;
        MDHostReview *review = [worker resolve:address route:route cancelled:IsClosing data:(__bridge void *)self error:&error];
        return review ?: (id)error;
    } done:^(id value) {
        if ([value isKindOfClass:NSString.class]) [self note:value kind:MDStatusError]; else [self note:nil kind:MDStatusNone];
        if ([value isKindOfClass:MDHostReview.class]) [self presentReview:value];
    }];
}
/* First-contact and key-change reviews. Both are trust decisions, so no
 * button answers Return; Escape cancels or keeps the saved key. A changed key
 * offers only Keep Old Key or Replace Key, which needs the exact fingerprint. */
- (void)presentReview:(MDHostReview *)review {
    NSWindow *window = self.view.window;
    if (!window || window.attachedSheet || self.closing) return;
    BOOL mismatch = review.oldKey != nil;
    NSUInteger generation = _generation;
    BOOL canSave = _book.canEdit && (!mismatch || [_book canReplaceReview:review]);
    NSString *alias = review.alias ? [MDHostBoundText(review.alias, 320) stringByAppendingString:@" — verified now"] : @"None";
    NSMutableArray *rows = [NSMutableArray array];
    NSTextField *confirmation = nil;
    __block id observer = nil;
    MDSheet *sheet;
    if (mismatch) {
        sheet = [[MDSheet alloc] initWithTitle:@"This Host’s Key Has Changed"
            message:@"The key published for this host no longer matches the one you saved, so connecting is blocked. "
                    @"Keep the saved key unless the host’s owner confirms they changed it."
              width:540];
        [rows addObject:@[@"Saved fingerprint", MDKeyValue(MDHostFingerprint(review.oldKey))]];
        [rows addObject:@[@"New fingerprint", MDKeyValue(review.fingerprint)]];
        [rows addObject:@[@"New public key", MDKeyValue(review.npub)]];
        [rows addObject:@[@"NIP-05", alias]];
    } else {
        sheet = [[MDSheet alloc] initWithTitle:@"Verify This Host"
            message:@"Before you connect or save this host, ask its owner to read you the fingerprint their Metadesk shows, and check that it matches."
              width:540];
        [rows addObject:@[@"Fingerprint", MDKeyValue(review.fingerprint)]];
        [rows addObject:@[@"Public key", MDKeyValue(review.npub)]];
        [rows addObject:@[@"NIP-05", alias]];
        [rows addObject:@[@"Requests", MDHostCapsText(review.caps)]];
        if (review.route.length) [rows addObject:@[@"Route hint", MDHostBoundText(review.route, 64)]];
    }
    NSGridView *details = MDDetails(rows);
    NSMutableArray *content = [NSMutableArray arrayWithObject:details];
    NSButton *replace = nil;
    if (mismatch) {
        [sheet addButton:@"Keep Old Key" role:MDSheetCancel response:NSAlertFirstButtonReturn];
        if (canSave) {
            confirmation = Field(@"Type the new fingerprint exactly", nil);
            confirmation.accessibilityLabel = @"New fingerprint, typed to confirm";
            confirmation.font = [NSFont monospacedSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
            NSTextField *hint = MDNote(@"To replace the saved key, type the new fingerprint exactly as the host’s owner reads it to you. You’ll review the host again before connecting.");
            [content addObject:MDForm(@[@[@"Confirm", confirmation], @[NSNull.null, hint]])];
            replace = [sheet addButton:@"Replace Key" role:MDSheetDestructive response:NSAlertSecondButtonReturn];
            replace.enabled = NO;
            MDHostsBook *book = _book;
            __weak NSButton *weakReplace = replace;
            observer = [NSNotificationCenter.defaultCenter addObserverForName:NSControlTextDidChangeNotification object:confirmation queue:nil
                usingBlock:^(NSNotification *note) { weakReplace.enabled = [book canConfirmReview:review fingerprint:((NSTextField *)note.object).stringValue]; }];
            sheet.initialFirstResponder = confirmation;
        }
    } else {
        [sheet addButton:@"Cancel" role:MDSheetCancel response:NSAlertFirstButtonReturn];
        if (canSave) [sheet addButton:@"Save Host" role:MDSheetOther response:NSAlertThirdButtonReturn];
        [sheet addButton:@"Connect" role:MDSheetAction response:NSAlertSecondButtonReturn];
    }
    NSStackView *body = MDColumn(content, MDGroupSpacing);
    if (content.count > 1) MDAlignForms(@[details, content[1]]);
    sheet.accessory = body;
    [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (observer) [NSNotificationCenter.defaultCenter removeObserver:observer];
        if (self.closing || generation != self->_generation) return;
        if (mismatch ? response == NSAlertSecondButtonReturn && canSave : response == NSAlertThirdButtonReturn && canSave) {
            NSString *value = confirmation.stringValue;
            if (mismatch && ![self->_book canConfirmReview:review fingerprint:value]) {
                [self note:@"The saved key was kept. Replacing it needs the exact new fingerprint." kind:MDStatusError]; return;
            }
            [self run:^id(MDHostsWorker *worker) { return [worker saveReview:review confirmation:value]; }
                done:^(NSString *message) { [self noteResult:message success:mismatch ? @"Key replaced. Review the host again to connect." : @"Host saved."]; }];
        } else if (!mismatch && response == NSAlertSecondButtonReturn) [self startLaunch:review];
    }];
}
- (void)startLaunch:(MDHostReview *)review {
    if (_launched || _busy) return;
    [self run:^id(MDHostsWorker *worker) {
        NSString *error = nil;
        NSMutableData *config = [NSMutableData dataWithLength:sizeof(MdManagedLaunch)];
        return [worker launchConfig:config.mutableBytes review:review error:&error] ? config : (id)error;
    } done:^(id value) {
        if (![value isKindOfClass:NSData.class]) { [self note:value ?: @"Connection unavailable." kind:MDStatusError]; return; }
        NSData *data = value; const MdManagedLaunch *config = data.bytes;
        self->_launched = YES; self->_phase = 0; self->_diagnosticText = nil;
        self->_control.title = @"Cancel Connection";
        [self note:nil kind:MDStatusNone];
        [self showConnection:MDHostLaunchText(0, 0) kind:MDStatusWorking];
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
        if (reason.length) {
            _diagnosticText = _diagnosticText.length ? [@[_diagnosticText, reason] componentsJoinedByString:@" "] : reason;
            [self showConnection:MDHostLaunchText(_phase, _phase) kind:_connection.kind];
        }
        return;
    }
    NSString *phase = MDHostLaunchText(m, _phase);
    if (m == MD_LAUNCH_CONNECTED || m == MD_LAUNCH_FAILED) {
        MdRecentOutcome outcome = MDHostLaunchOutcome(m, _phase);
        dispatch_async(_queue, ^{ @autoreleasepool {
            if (!self.closing) { [self->_worker recordKey:key owner:owner outcome:outcome]; [self signalChanged]; }
        } });
    }
    MDStatusKind kind = MDStatusWorking;
    if (m == MD_LAUNCH_CONNECTED) { _control.title = @"Disconnect"; _diagnosticText = nil; kind = MDStatusOK; }
    else if (m >= MD_LAUNCH_FAILED) {
        _launched = NO; [_launch close:nil]; _launch = nil;
        if (m != MD_LAUNCH_FAILED) _diagnosticText = nil;
        kind = m == MD_LAUNCH_FAILED ? MDStatusError : MDStatusInfo;
    }
    [self showConnection:phase kind:kind];
    if (m <= MD_LAUNCH_CONNECTED) _phase = m;
    [self render];
}
- (void)control:(id)sender { (void)sender; [_launch control]; }
- (void)edit:(id)sender {
    (void)sender;
    NSDictionary *row = self.selected;
    NSWindow *window = self.view.window;
    if (!_book.canEdit || _busy || !row || !window || window.attachedSheet) return;
    MDHostsBook *book = _book;
    NSUInteger generation = _generation;
    NSString *key = row[@"key"];
    MDSheet *sheet = [[MDSheet alloc] initWithTitle:[NSString stringWithFormat:@"Edit “%@”", MDHostBoundText(row[@"title"], 64)]
        message:@"The label, notes and tags are stored encrypted in your address book." width:520];
    NSTextField *label = Field(@"Name shown in your host list", row[@"label"]), *notes = Field(@"Optional", row[@"notes"]);
    label.accessibilityLabel = @"Label"; notes.accessibilityLabel = @"Notes";
    NSTextField *newTag = Field(@"Add a new tag (optional)", nil); newTag.accessibilityLabel = @"New tag";
    NSButton *favorite = [NSButton checkboxWithTitle:@"Favorite — show first in Quick Connect" target:nil action:NULL];
    favorite.state = [row[@"favorite"] boolValue] ? NSControlStateValueOn : NSControlStateValueOff;
    MDHostChoices *choices = [[MDHostChoices alloc] initWithRows:book.tags importing:NO selected:[NSSet setWithArray:row[@"tagIDs"] ?: @[]]];
    NSMutableArray *form = [NSMutableArray arrayWithArray:@[@[@"Label", label], @[@"Notes", notes], @[NSNull.null, favorite]]];
    if (book.tags.count) [form addObject:@[@"Tags", choices.view]];
    [form addObject:@[book.tags.count ? NSNull.null : @"Tags", newTag]];
    sheet.accessory = MDForm(form);
    sheet.initialFirstResponder = label;
    [sheet addButton:@"Remove Host…" role:MDSheetDestructive response:NSAlertThirdButtonReturn];
    [sheet addButton:@"Cancel" role:MDSheetCancel response:NSAlertFirstButtonReturn];
    [sheet addButton:@"Save" role:MDSheetDefault response:NSAlertSecondButtonReturn];
    [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (self.closing || generation != self->_generation || response == NSAlertFirstButtonReturn) return;
        void (^edit)(BOOL) = ^(BOOL remove) {
            NSString *name = label.stringValue, *memo = notes.stringValue, *tag = MDHostTrim(newTag.stringValue);
            BOOL fav = favorite.state == NSControlStateValueOn;
            NSSet *membership = [choices keys];
            [self run:^id(MDHostsWorker *worker) {
                return [worker editKey:key book:book label:name notes:memo favorite:fav tagIDs:membership newTag:tag remove:remove];
            } done:^(NSString *message) { [self noteResult:message success:remove ? @"Host removed." : @"Host updated."]; }];
        };
        if (response == NSAlertSecondButtonReturn) edit(NO);
        else if (response == NSAlertThirdButtonReturn && self.view.window) {
            NSAlert *remove = MDAlert([NSString stringWithFormat:@"Remove “%@” from Your Hosts?", MDHostBoundText(row[@"title"], 64)],
                @"This removes the saved host, its notes and its tags from your address book. It doesn’t revoke any access the host has given you.");
            MDAlertDestructive(remove, @"Remove Host");
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
            done:^(NSString *message) { [self noteResult:message success:@"Pending edit synced."]; }];
        return;
    }
    [self run:^id(MDHostsWorker *worker) { (void)worker; return [book conflictText:coordinate]; }
        done:^(NSString *text) {
        if (!text || !self.view.window) return;
        NSWindow *window = self.view.window;
        if (window.attachedSheet) return;
        MDSheet *sheet = [[MDSheet alloc] initWithTitle:@"Resolve a Sync Conflict"
            message:@"Your edit and a newer version from another device changed the same part of your address book. "
                    @"Compare them, then keep the newer version or reapply your edit on top of it." width:600];
        NSScrollView *scroll = [NSScrollView new];
        scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES; scroll.borderType = NSBezelBorder;
        NSTextView *display = [NSTextView new];
        display.editable = NO; display.selectable = YES; display.richText = NO;
        display.horizontallyResizable = NO; display.verticallyResizable = YES;
        display.autoresizingMask = NSViewWidthSizable;
        display.textContainer.widthTracksTextView = YES;
        display.textContainerInset = NSMakeSize(6, 6);
        display.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
        display.textColor = NSColor.textColor; display.backgroundColor = NSColor.textBackgroundColor;
        display.string = MDHostBoundText(text, 65536);
        display.accessibilityLabel = @"Both versions";
        scroll.documentView = display;
        [scroll.heightAnchor constraintEqualToConstant:240].active = YES;
        sheet.accessory = scroll;
        [sheet addButton:@"Cancel" role:MDSheetCancel response:NSAlertFirstButtonReturn];
        [sheet addButton:@"Keep Newer Version" role:MDSheetAction response:NSAlertSecondButtonReturn];
        [sheet addButton:@"Reapply My Edit" role:MDSheetAction response:NSAlertThirdButtonReturn];
        [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
            if (self.closing || (response != NSAlertSecondButtonReturn && response != NSAlertThirdButtonReturn)) return;
            [self run:^id(MDHostsWorker *worker) { return [worker resolveIssue:coordinate book:book reapply:response == NSAlertThirdButtonReturn]; }
                done:^(NSString *message) { [self noteResult:message success:@"Conflict resolved."]; }];
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
    panel.message = @"Choose a metadesk-hosts/1 file exported from Metadesk.";
    panel.prompt = @"Review";
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (response != NSModalResponseOK || self.closing || generation != self->_generation) return;
        NSURL *url = panel.URL;
        [self run:^id(MDHostsWorker *worker) { return [worker readImport:url book:book]; }
            done:^(NSArray *rows) {
            if (!rows.count) { [self note:rows ? @"That file has no hosts you haven’t already saved." : @"That file isn’t a metadesk-hosts/1 hosts file, or it couldn’t be read." kind:rows ? MDStatusInfo : MDStatusError]; return; }
            [self reviewImport:rows book:book];
        }];
    }];
}
- (void)reviewImport:(NSArray *)rows book:(MDHostsBook *)book {
    NSWindow *window = self.view.window;
    if (!window || window.attachedSheet) return;
    MDSheet *sheet = [[MDSheet alloc] initWithTitle:@"Import Hosts"
        message:@"Choose only hosts whose fingerprints you recognize. NIP-05 names in a file aren’t verified, and hosts you’ve already saved won’t change."
          width:560];
    MDHostChoices *choices = [[MDHostChoices alloc] initWithRows:rows importing:YES selected:[NSSet set]];
    sheet.accessory = choices.view;
    [sheet addButton:@"Cancel" role:MDSheetCancel response:NSAlertFirstButtonReturn];
    NSButton *importButton = [sheet addButton:@"Import Selected" role:MDSheetAction response:NSAlertSecondButtonReturn];
    importButton.enabled = NO;
    __weak MDHostChoices *weakChoices = choices; __weak NSButton *weakImport = importButton;
    choices.changed = ^{ weakImport.enabled = weakChoices.keys.count != 0; };
    [sheet beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (response != NSAlertSecondButtonReturn || self.closing) return;
        NSSet *keys = choices.keys;
        if (!keys.count) return;
        [self run:^id(MDHostsWorker *worker) { return [worker applyImport:rows selected:keys book:book]; }
            done:^(NSString *message) { [self noteResult:message success:keys.count == 1 ? @"1 host imported." : [NSString stringWithFormat:@"%lu hosts imported.", (unsigned long)keys.count]]; }];
    }];
}
- (void)exportHosts:(id)sender {
    (void)sender;
    if (_busy || !_book.canExport || !self.view.window || self.view.window.attachedSheet) return;
    MDHostsBook *book = _book;
    NSUInteger generation = _generation;
    NSAlert *alert = MDAlert(@"Export Hosts as Plain Text?",
        @"Anyone who can read the file will see your hosts’ public keys, labels and notes. It doesn’t contain anything that lets someone connect, and no access grants.");
    MDAlertConfirm(alert, @"Export…");
    [alert beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        if (response != NSAlertFirstButtonReturn || self.closing || generation != self->_generation || !self.view.window) return;
        NSSavePanel *panel = [NSSavePanel savePanel]; panel.nameFieldStringValue = @"metadesk-hosts.json";
        panel.allowedContentTypes = @[UTTypeJSON];
        panel.message = @"The export is plain text.";
        [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse selected) {
            if (selected != NSModalResponseOK || self.closing || generation != self->_generation) return;
            NSURL *url = panel.URL;
            [self run:^id(MDHostsWorker *worker) { return [worker exportBook:book to:url]; }
                done:^(NSString *message) { [self note:message kind:[message isEqual:@"Plaintext address book exported."] ? MDStatusOK : MDStatusError]; }];
        }];
    }];
}
@end
