#import "main_window.h"

static NSString *const RefreshItem = @"dev.metadesk.manager.refresh";
static NSString *const SearchItem = @"dev.metadesk.manager.search";

@implementation MDSection
+ (instancetype)section:(NSString *)identifier symbol:(NSString *)symbol controller:(NSViewController *)controller view:(NSView *)view {
    MDSection *s = [MDSection new];
    s->_identifier = [identifier copy]; s->_symbol = [symbol copy]; s->_controller = controller; s->_view = view ?: controller.view;
    return s;
}
@end

@interface MDSidebarCell : NSTableCellView
@property(nonatomic, readonly) NSTextField *badgeField;
@end
@implementation MDSidebarCell
- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    NSImageView *icon = [NSImageView new];
    NSTextField *title = [NSTextField labelWithString:@""];
    title.lineBreakMode = NSLineBreakByTruncatingTail;
    [title setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _badgeField = [NSTextField labelWithString:@""];
    _badgeField.textColor = NSColor.secondaryLabelColor;
    _badgeField.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
    [_badgeField setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_badgeField setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    for (NSView *v in @[icon, title, _badgeField]) { v.translatesAutoresizingMaskIntoConstraints = NO; [self addSubview:v]; }
    self.imageView = icon; self.textField = title;
    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2],
        [icon.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:20],
        [title.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:6],
        [title.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_badgeField.leadingAnchor constraintGreaterThanOrEqualToAnchor:title.trailingAnchor constant:6],
        [_badgeField.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-6],
        [_badgeField.firstBaselineAnchor constraintEqualToAnchor:title.firstBaselineAnchor]]];
    return self;
}
@end

@interface MDSidebarController : NSViewController <NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic) NSArray<MDSection *> *sections;
@property(nonatomic, readonly) NSTableView *table;
@property(nonatomic, copy) void (^selected)(MDSection *section);
@end
@implementation MDSidebarController
- (void)loadView {
    _table = [NSTableView new];
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"Section"];
    [_table addTableColumn:column];
    _table.headerView = nil; _table.style = NSTableViewStyleSourceList;
    _table.allowsEmptySelection = NO; _table.allowsMultipleSelection = NO;
    _table.dataSource = self; _table.delegate = self;
    _table.accessibilityLabel = @"Sections";
    NSScrollView *scroll = [NSScrollView new];
    scroll.documentView = _table; scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    self.view = scroll;
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { (void)table; return (NSInteger)self.sections.count; }
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)column;
    MDSidebarCell *cell = [table makeViewWithIdentifier:@"SectionCell" owner:nil];
    if (!cell) { cell = [MDSidebarCell new]; cell.identifier = @"SectionCell"; }
    MDSection *s = self.sections[(NSUInteger)row];
    cell.textField.stringValue = s.identifier;
    cell.imageView.image = [NSImage imageWithSystemSymbolName:s.symbol accessibilityDescription:nil];
    cell.badgeField.stringValue = s.badge ?: @"";
    cell.badgeField.hidden = !s.badge.length;
    cell.accessibilityLabel = s.badge.length ? [NSString stringWithFormat:@"%@, %@", s.identifier, s.badge] : s.identifier;
    return cell;
}
- (NSString *)tableView:(NSTableView *)table typeSelectStringForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table; (void)column; return self.sections[(NSUInteger)row].identifier;
}
- (void)tableViewSelectionDidChange:(NSNotification *)note {
    (void)note;
    NSInteger row = _table.selectedRow;
    if (row >= 0 && self.selected) self.selected(self.sections[(NSUInteger)row]);
}
@end

@interface MDContentController : NSViewController
- (void)show:(MDSection *)section;
@end
@implementation MDContentController {
    MDSection *_current;
}
- (void)loadView { self.view = [NSView new]; }
- (void)show:(MDSection *)section {
    if (_current == section) return;
    if (_current) {
        [_current.view removeFromSuperview];
        if (_current.controller) [_current.controller removeFromParentViewController];
    }
    _current = section;
    if (section.controller) [self addChildViewController:section.controller];
    NSView *v = section.view;
    v.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:v];
    NSLayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [v.topAnchor constraintEqualToAnchor:safe.topAnchor], [v.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [v.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor], [v.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor]]];
}
@end

@implementation MDMainWindowController {
    MDSidebarController *_sidebar;
    MDContentController *_content;
    NSSplitViewController *_split;
    NSSearchToolbarItem *_search;
    MDSection *_current;
    BOOL _switching;
}
+ (NSSize)defaultContentSize { return NSMakeSize(900, 620); }
+ (NSSize)minimumContentSize { return NSMakeSize(720, 540); }
- (instancetype)initWithSections:(NSArray<MDSection *> *)sections {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:(NSRect){NSZeroPoint, MDMainWindowController.defaultContentSize}
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                  NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView
        backing:NSBackingStoreBuffered defer:YES];
    if (!(self = [super initWithWindow:window])) return nil;
    _sections = [sections copy];
    window.releasedWhenClosed = NO;
    window.restorable = NO; /* frame only: never restore page content */
    window.title = @"Metadesk Manager";
    window.tabbingMode = NSWindowTabbingModeDisallowed;
    /* Tab follows the visual order: sidebar, page controls, tables, toolbar. */
    window.autorecalculatesKeyViewLoop = YES;
    window.delegate = self;

    _sidebar = [MDSidebarController new];
    _sidebar.sections = _sections;
    __weak MDMainWindowController *weakSelf = self;
    _sidebar.selected = ^(MDSection *section) { [weakSelf select:section]; };
    _content = [MDContentController new];
    NSSplitViewItem *side = [NSSplitViewItem sidebarWithViewController:_sidebar];
    side.minimumThickness = 170; side.maximumThickness = 260; side.canCollapse = YES;
    side.allowsFullHeightLayout = YES;
    NSSplitViewItem *main = [NSSplitViewItem splitViewItemWithViewController:_content];
    main.minimumThickness = 520;
    _split = [NSSplitViewController new];
    _split.splitView.vertical = YES; _split.splitView.dividerStyle = NSSplitViewDividerStyleThin;
    _split.splitView.autosaveName = @"MetadeskManagerSidebar";
    [_split addSplitViewItem:side]; [_split addSplitViewItem:main];
    window.contentViewController = _split;

    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"MetadeskManagerToolbar"];
    toolbar.delegate = self; toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    toolbar.allowsUserCustomization = NO; toolbar.autosavesConfiguration = NO;
    window.toolbar = toolbar;
    window.toolbarStyle = NSWindowToolbarStyleUnified;

    [window setContentSize:MDMainWindowController.defaultContentSize];
    window.contentMinSize = MDMainWindowController.minimumContentSize;
    [window center];
    [window setFrameAutosaveName:@"MetadeskManagerMainWindow"];
    return self;
}
- (NSString *)currentSection { return _current.identifier; }
- (NSSearchField *)searchField { return _search.searchField; }
- (void)showSection:(NSString *)identifier {
    for (NSUInteger i = 0; i < _sections.count; ++i) if ([_sections[i].identifier isEqual:identifier]) {
        (void)self.window; (void)_sidebar.view;
        [_sidebar.table reloadData];
        [_sidebar.table selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
        [self select:_sections[i]];
        return;
    }
}
- (void)select:(MDSection *)section {
    if (_switching || section == _current) return;
    _switching = YES;
    _current = section;
    [_content show:section];
    self.window.title = section.identifier;
    [self updateToolbar];
    _switching = NO;
    if (self.sectionChanged) self.sectionChanged(section.identifier);
}
- (void)focusSearch {
    if (!_current.searchable || !_search) return;
    [self.window makeFirstResponder:_search.searchField];
}
- (void)setBadge:(NSString *)badge forSection:(NSString *)identifier {
    for (NSUInteger i = 0; i < _sections.count; ++i) if ([_sections[i].identifier isEqual:identifier]) {
        if ([_sections[i].badge isEqual:badge] || (!_sections[i].badge && !badge)) return;
        _sections[i].badge = badge;
        [_sidebar.table reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:i] columnIndexes:[NSIndexSet indexSetWithIndex:0]];
    }
}
- (void)updateToolbar {
    NSToolbar *toolbar = self.window.toolbar;
    for (NSInteger i = (NSInteger)toolbar.items.count - 1; i >= 0; --i) {
        NSString *identifier = toolbar.items[(NSUInteger)i].itemIdentifier;
        if ([identifier isEqual:RefreshItem] || [identifier isEqual:SearchItem]) [toolbar removeItemAtIndex:i];
    }
    if (_current.refreshHelp) [toolbar insertItemWithItemIdentifier:RefreshItem atIndex:(NSInteger)toolbar.items.count];
    if (_current.searchable) [toolbar insertItemWithItemIdentifier:SearchItem atIndex:(NSInteger)toolbar.items.count];
    for (NSToolbarItem *item in toolbar.items) if ([item.itemIdentifier isEqual:RefreshItem]) item.toolTip = _current.refreshHelp;
}
- (void)searchFieldChanged:(NSSearchField *)field {
    if (self.searchChanged) self.searchChanged(field.stringValue);
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar {
    (void)toolbar;
    return @[NSToolbarToggleSidebarItemIdentifier, NSToolbarSidebarTrackingSeparatorItemIdentifier, NSToolbarFlexibleSpaceItemIdentifier];
}
- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar {
    (void)toolbar;
    return @[NSToolbarToggleSidebarItemIdentifier, NSToolbarSidebarTrackingSeparatorItemIdentifier, NSToolbarFlexibleSpaceItemIdentifier,
             RefreshItem, SearchItem];
}
- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar itemForItemIdentifier:(NSToolbarItemIdentifier)identifier willBeInsertedIntoToolbar:(BOOL)flag {
    (void)toolbar; (void)flag;
    if ([identifier isEqual:RefreshItem]) {
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
        item.label = @"Refresh"; item.paletteLabel = @"Refresh";
        item.image = [NSImage imageWithSystemSymbolName:@"arrow.clockwise" accessibilityDescription:@"Refresh"];
        item.action = @selector(refresh:); item.target = nil; item.bordered = YES; item.autovalidates = YES;
        item.toolTip = _current.refreshHelp;
        return item;
    }
    if ([identifier isEqual:SearchItem]) {
        if (!_search) {
            _search = [[NSSearchToolbarItem alloc] initWithItemIdentifier:identifier];
            _search.label = @"Search"; _search.paletteLabel = @"Search Hosts";
            _search.preferredWidthForSearchField = 220;
            NSSearchField *field = _search.searchField;
            field.placeholderString = @"Search Hosts";
            field.accessibilityLabel = @"Search saved hosts";
            field.sendsSearchStringImmediately = YES; field.sendsWholeSearchString = NO;
            field.target = self; field.action = @selector(searchFieldChanged:);
        }
        return _search;
    }
    return nil;
}
@end
