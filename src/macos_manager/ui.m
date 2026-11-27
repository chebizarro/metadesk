#import "ui.h"

static void Shrinkable(NSView *view) {
    [view setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
}
NSTextField *MDLabel(NSString *text) {
    NSTextField *f = [NSTextField labelWithString:text ?: @""];
    f.lineBreakMode = NSLineBreakByTruncatingTail;
    Shrinkable(f);
    return f;
}
NSTextField *MDWrappingLabel(NSString *text) {
    NSTextField *f = [NSTextField wrappingLabelWithString:text ?: @""];
    f.selectable = NO;
    Shrinkable(f);
    return f;
}
NSTextField *MDNote(NSString *text) {
    NSTextField *f = MDWrappingLabel(text);
    f.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    f.textColor = NSColor.secondaryLabelColor;
    return f;
}
NSTextField *MDHeader(NSString *text) {
    NSTextField *f = MDLabel(text);
    f.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    f.accessibilityRole = NSAccessibilityStaticTextRole;
    return f;
}
NSTextField *MDFormLabel(NSString *text) {
    NSTextField *f = [NSTextField labelWithString:[(text ?: @"") stringByAppendingString:@":"]];
    f.alignment = NSTextAlignmentRight;
    [f setContentHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationHorizontal];
    return f;
}
NSTextField *MDValue(NSString *text) {
    NSTextField *f = MDLabel(text);
    f.selectable = YES; f.lineBreakMode = NSLineBreakByTruncatingMiddle;
    f.toolTip = text.length ? text : nil;
    return f;
}
NSTextField *MDWrappingValue(NSString *text) {
    NSTextField *f = MDWrappingLabel(text);
    f.selectable = YES;
    return f;
}

/* Keys, names and relay URLs must reach the model exactly as typed or pasted,
 * so this field editor turns off completion, correction and substitutions.
 * Editing itself is standard: cut, copy, paste, select all, undo, drag and
 * Services all work through the responder chain. What the user can see is
 * theirs to copy; secret-key input is refused by the pages that classify it. */
@interface MDKeyFieldCell : NSTextFieldCell
@end
@implementation MDKeyFieldCell {
    NSTextView *_editor;
}
- (NSTextView *)fieldEditorForView:(NSView *)control {
    (void)control;
    if (!_editor) {
        _editor = [[NSTextView alloc] initWithFrame:NSZeroRect];
        _editor.fieldEditor = YES; _editor.allowsUndo = YES; _editor.richText = NO;
        _editor.automaticTextCompletionEnabled = NO;
        _editor.continuousSpellCheckingEnabled = NO;
        _editor.automaticSpellingCorrectionEnabled = NO;
        _editor.automaticQuoteSubstitutionEnabled = NO;
        _editor.automaticDashSubstitutionEnabled = NO;
        _editor.automaticTextReplacementEnabled = NO;
        _editor.automaticDataDetectionEnabled = NO;
        _editor.automaticLinkDetectionEnabled = NO;
    }
    return _editor;
}
@end
NSTextField *MDKeyField(NSString *placeholder, NSString *accessibilityLabel) {
    NSTextField *field = [[NSTextField alloc] initWithFrame:NSZeroRect];
    MDKeyFieldCell *cell = [[MDKeyFieldCell alloc] initTextCell:@""];
    cell.editable = YES; cell.selectable = YES; cell.bezeled = YES; cell.bezelStyle = NSTextFieldSquareBezel;
    cell.drawsBackground = YES; cell.backgroundColor = NSColor.textBackgroundColor; cell.textColor = NSColor.controlTextColor;
    cell.scrollable = YES; cell.wraps = NO; cell.usesSingleLineMode = YES;
    cell.sendsActionOnEndEditing = NO; cell.placeholderString = placeholder;
    cell.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    field.cell = cell;
    field.accessibilityLabel = accessibilityLabel ?: placeholder;
    [field setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    Shrinkable(field);
    [field.widthAnchor constraintGreaterThanOrEqualToConstant:120].active = YES;
    return field;
}
NSButton *MDButton(NSString *title, id target, SEL action) {
    NSButton *b = [NSButton buttonWithTitle:title target:target action:action];
    [b setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh + 1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    return b;
}
NSButton *MDSymbolButton(NSString *symbol, NSString *accessibilityLabel, id target, SEL action) {
    NSImage *image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:accessibilityLabel];
    NSButton *b = [NSButton buttonWithImage:image target:target action:action];
    b.bezelStyle = NSBezelStyleSmallSquare; b.title = @""; b.imagePosition = NSImageOnly;
    b.accessibilityLabel = accessibilityLabel; b.toolTip = accessibilityLabel;
    [b.widthAnchor constraintGreaterThanOrEqualToConstant:24].active = YES;
    return b;
}
NSStackView *MDRow(NSArray<NSView *> *views) {
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.alignment = NSLayoutAttributeFirstBaseline; s.spacing = MDItemSpacing;
    return s;
}
NSStackView *MDColumn(NSArray<NSView *> *views, CGFloat spacing) {
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading; s.spacing = spacing;
    for (NSView *v in views) if (![v isKindOfClass:NSButton.class])
        [v.widthAnchor constraintEqualToAnchor:s.widthAnchor].active = YES;
    return s;
}
void MDAlignForms(NSArray<NSGridView *> *forms);
static BOOL Fills(NSView *view) {
    if ([view isKindOfClass:NSTextField.class]) return YES;
    if ([view isKindOfClass:NSStackView.class] || [view isKindOfClass:NSScrollView.class] || [view isKindOfClass:NSBox.class]) return YES;
    return [view isMemberOfClass:NSView.class];
}
NSGridView *MDForm(NSArray<NSArray *> *rows) {
    NSGridView *grid = [NSGridView gridViewWithNumberOfColumns:2 rows:0];
    grid.columnSpacing = MDItemSpacing; grid.rowSpacing = MDRowSpacing;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementFill;
    for (NSArray *row in rows) {
        if (row.count == 1) {
            NSGridRow *r = [grid addRowWithViews:@[row[0], NSGridCell.emptyContentView]];
            [r mergeCellsInRange:NSMakeRange(0, 2)];
            [r cellAtIndex:0].xPlacement = Fills(row[0]) ? NSGridCellPlacementFill : NSGridCellPlacementLeading;
            continue;
        }
        id label = row[0];
        NSView *labelView = label == NSNull.null ? NSGridCell.emptyContentView :
            [label isKindOfClass:NSString.class] ? MDFormLabel(label) : label;
        NSGridRow *r = [grid addRowWithViews:@[labelView, row[1]]];
        if (!Fills(row[1])) [r cellAtIndex:1].xPlacement = NSGridCellPlacementLeading;
        else [(NSView *)row[1] setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    MDAlignForms(@[grid]);
    return grid;
}
void MDAlignForms(NSArray<NSGridView *> *forms) {
    CGFloat width = 0;
    for (NSGridView *g in forms) for (NSInteger r = 0; r < g.numberOfRows; ++r) {
        NSView *v = [g cellAtColumnIndex:0 rowIndex:r].contentView;
        if (v && v != NSGridCell.emptyContentView && [g cellAtColumnIndex:0 rowIndex:r].row.numberOfCells && v.intrinsicContentSize.width > width
            && [g cellAtColumnIndex:1 rowIndex:r].contentView != v) width = v.intrinsicContentSize.width;
    }
    for (NSGridView *g in forms) [g columnAtIndex:0].width = ceil(width);
}
void MDSetRowHidden(NSView *view, BOOL hidden) {
    view.hidden = hidden;
    NSView *v = view;
    while (v.superview && ![v.superview isKindOfClass:NSGridView.class]) v = v.superview;
    NSGridView *grid = (NSGridView *)v.superview;
    if (grid) [grid cellForView:v].row.hidden = hidden;
}
NSBox *MDSeparator(void) { NSBox *b = [NSBox new]; b.boxType = NSBoxSeparator; return b; }
NSView *MDPage(NSArray<NSView *> *views, NSStackView **out) {
    NSView *root = [NSView new];
    NSStackView *stack = [NSStackView stackViewWithViews:@[]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading; stack.spacing = MDRowSpacing;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    BOOL flexible = NO;
    for (NSView *v in views) {
        [stack addArrangedSubview:v];
        if (![v isKindOfClass:NSButton.class]) [v.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
        if ([v contentHuggingPriorityForOrientation:NSLayoutConstraintOrientationVertical] <= 1) flexible = YES;
    }
    [root addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.topAnchor constant:MDMargin],
        [stack.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:MDMargin],
        [stack.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-MDMargin],
        flexible ? [stack.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-MDMargin]
                 : [stack.bottomAnchor constraintLessThanOrEqualToAnchor:root.bottomAnchor constant:-MDMargin]]];
    if (out) *out = stack;
    return root;
}
@interface MDFlippedView : NSView
@end
@implementation MDFlippedView
- (BOOL)isFlipped { return YES; }
@end
NSView *MDScrollingPage(NSView *page) {
    NSScrollView *scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES; scroll.drawsBackground = NO;
    scroll.borderType = NSNoBorder;
    MDFlippedView *document = [MDFlippedView new];
    document.translatesAutoresizingMaskIntoConstraints = NO;
    page.translatesAutoresizingMaskIntoConstraints = NO;
    [document addSubview:page];
    scroll.documentView = document;
    NSClipView *clip = scroll.contentView;
    [NSLayoutConstraint activateConstraints:@[
        [page.topAnchor constraintEqualToAnchor:document.topAnchor], [page.bottomAnchor constraintEqualToAnchor:document.bottomAnchor],
        [page.leadingAnchor constraintEqualToAnchor:document.leadingAnchor], [page.trailingAnchor constraintEqualToAnchor:document.trailingAnchor],
        [document.topAnchor constraintEqualToAnchor:clip.topAnchor], [document.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
        [document.widthAnchor constraintEqualToAnchor:clip.widthAnchor],
        /* Fill a tall window so a flexible table in the page can grow. */
        [document.heightAnchor constraintGreaterThanOrEqualToAnchor:clip.heightAnchor]]];
    return scroll;
}
NSScrollView *MDTableScroll(NSTableView *table) {
    NSScrollView *scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES; scroll.hasHorizontalScroller = YES;
    scroll.autohidesScrollers = YES; scroll.borderType = NSBezelBorder;
    scroll.documentView = table;
    table.usesAlternatingRowBackgroundColors = NO;
    table.style = NSTableViewStyleFullWidth;
    table.usesAutomaticRowHeights = YES;
    table.allowsTypeSelect = YES;
    [scroll setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
    [scroll setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:96].active = YES;
    return scroll;
}
NSView *MDTableArea(NSScrollView *scroll, NSView **overlayOut, NSTextField **title, NSTextField **detail) {
    NSView *area = [NSView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [area addSubview:scroll];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:area.topAnchor], [scroll.bottomAnchor constraintEqualToAnchor:area.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:area.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:area.trailingAnchor]]];
    [area setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
    NSTextField *t = [NSTextField labelWithString:@""];
    t.font = [NSFont systemFontOfSize:NSFont.systemFontSize + 2 weight:NSFontWeightMedium];
    t.textColor = NSColor.secondaryLabelColor; t.alignment = NSTextAlignmentCenter;
    NSTextField *d = MDNote(@""); d.alignment = NSTextAlignmentCenter;
    NSStackView *overlay = [NSStackView stackViewWithViews:@[t, d]];
    overlay.orientation = NSUserInterfaceLayoutOrientationVertical; overlay.spacing = 4;
    overlay.translatesAutoresizingMaskIntoConstraints = NO;
    [area addSubview:overlay positioned:NSWindowAbove relativeTo:scroll];
    [NSLayoutConstraint activateConstraints:@[
        [overlay.centerXAnchor constraintEqualToAnchor:area.centerXAnchor],
        [overlay.centerYAnchor constraintEqualToAnchor:area.centerYAnchor constant:12],
        [overlay.widthAnchor constraintLessThanOrEqualToAnchor:area.widthAnchor constant:-40],
        [d.widthAnchor constraintLessThanOrEqualToConstant:360]]];
    if (overlayOut) *overlayOut = overlay;
    if (title) *title = t;
    if (detail) *detail = d;
    return area;
}
NSTableColumn *MDColumnNamed(NSTableView *table, NSString *identifier, NSString *title, CGFloat width, NSString *sortKey) {
    NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:identifier];
    c.title = title; c.width = width; c.minWidth = MIN(width, 40);
    c.resizingMask = NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask;
    if (sortKey) c.sortDescriptorPrototype = [NSSortDescriptor sortDescriptorWithKey:sortKey ascending:YES selector:@selector(localizedStandardCompare:)];
    /* A table without a frame yet would shrink earlier columns as each is added. */
    table.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
    [table addTableColumn:c];
    return c;
}
@implementation MDTable {
    BOOL _armed;
}
- (void)arm { _armed = YES; }
- (void)setFrameSize:(NSSize)size {
    BOOL first = _armed && size.width > 1;
    if (first) { _armed = NO; self.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle; }
    [super setFrameSize:size];
}
@end
void MDFinishColumns(NSTableView *table, NSString *flexible) {
    for (NSTableColumn *c in table.tableColumns) {
        if ([c.identifier isEqual:flexible]) c.resizingMask |= NSTableColumnAutoresizingMask;
        else c.resizingMask &= ~NSTableColumnAutoresizingMask;
    }
    table.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
    if ([table isKindOfClass:MDTable.class]) [(id)table arm];
    else table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
}
NSTableCellView *MDCell(NSTableView *table, NSString *identifier, NSString *text) {
    NSTableCellView *cell = [table makeViewWithIdentifier:identifier owner:nil];
    if (!cell) {
        cell = [NSTableCellView new]; cell.identifier = identifier;
        NSTextField *f = MDLabel(@"");
        f.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:f]; cell.textField = f;
        [NSLayoutConstraint activateConstraints:@[
            [f.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:2],
            [f.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-2],
            [f.topAnchor constraintEqualToAnchor:cell.topAnchor constant:3],
            [f.bottomAnchor constraintEqualToAnchor:cell.bottomAnchor constant:-3]]];
    }
    cell.textField.stringValue = text ?: @"";
    cell.textField.toolTip = text.length ? text : nil;
    return cell;
}

@implementation MDStatusView {
    NSImageView *_icon;
    NSProgressIndicator *_spinner;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    self.alignment = NSLayoutAttributeCenterY; self.spacing = 6;
    _icon = [NSImageView new];
    _icon.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:NSFont.systemFontSize weight:NSFontWeightRegular];
    [_icon setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    _spinner = [NSProgressIndicator new];
    _spinner.style = NSProgressIndicatorStyleSpinning; _spinner.controlSize = NSControlSizeSmall;
    _spinner.displayedWhenStopped = NO; _spinner.indeterminate = YES;
    _label = MDWrappingLabel(@"");
    [self addArrangedSubview:_icon]; [self addArrangedSubview:_spinner]; [self addArrangedSubview:_label];
    [self setText:@"" kind:MDStatusNone];
    return self;
}
- (void)setText:(NSString *)text kind:(MDStatusKind)kind {
    _kind = kind;
    _label.stringValue = text ?: @"";
    NSString *symbol = nil, *description = nil; NSColor *tint = nil;
    switch (kind) {
        case MDStatusInfo: symbol = @"info.circle"; description = @"Information"; tint = NSColor.secondaryLabelColor; break;
        case MDStatusOK: symbol = @"checkmark.circle.fill"; description = @"OK"; tint = NSColor.systemGreenColor; break;
        case MDStatusWarning: symbol = @"exclamationmark.triangle.fill"; description = @"Warning"; tint = NSColor.systemOrangeColor; break;
        case MDStatusError: symbol = @"xmark.octagon.fill"; description = @"Error"; tint = NSColor.systemRedColor; break;
        default: break;
    }
    _icon.image = symbol ? [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:description] : nil;
    _icon.contentTintColor = tint;
    _icon.hidden = symbol == nil;
    if (kind == MDStatusWorking) { _spinner.hidden = NO; [_spinner startAnimation:nil]; }
    else { [_spinner stopAnimation:nil]; _spinner.hidden = YES; }
    MDSetRowHidden(self, !text.length);
    self.accessibilityLabel = description ? [NSString stringWithFormat:@"%@: %@", description, text ?: @""] : text;
}
@end

NSAlert *MDAlert(NSString *title, NSString *info) {
    NSAlert *a = [NSAlert new]; a.messageText = title; a.informativeText = info ?: @""; return a;
}
void MDAlertConfirm(NSAlert *alert, NSString *action) {
    [alert addButtonWithTitle:action];
    [alert addButtonWithTitle:@"Cancel"];
}
NSButton *MDAlertDestructive(NSAlert *alert, NSString *action) {
    alert.alertStyle = NSAlertStyleWarning;
    [alert addButtonWithTitle:@"Cancel"];
    NSButton *b = [alert addButtonWithTitle:action];
    b.hasDestructiveAction = YES; b.keyEquivalent = @"";
    return b;
}
NSTextField *MDKeyValue(NSString *text) {
    NSTextField *f = MDWrappingValue(text);
    f.font = [NSFont monospacedSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
    f.lineBreakMode = NSLineBreakByCharWrapping;
    return f;
}
NSGridView *MDDetails(NSArray<NSArray<NSString *> *> *rows) {
    NSMutableArray *grid = [NSMutableArray array];
    for (NSArray *row in rows) {
        id value = row[1];
        NSView *view = [value isKindOfClass:NSView.class] ? value : MDWrappingValue(value);
        [grid addObject:@[row[0], view]];
    }
    NSGridView *g = MDForm(grid);
    g.rowSpacing = 6;
    return g;
}

@class MDSheet;
@interface MDSheetWindow : NSWindow
@property(nonatomic, weak) MDSheet *owner;
@end
@interface MDSheet ()
- (void)cancelSheet;
@end
@implementation MDSheetWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (void)cancelOperation:(id)sender { (void)sender; [self.owner cancelSheet]; }
@end

@implementation MDSheet {
    NSStackView *_stack, *_buttons;
    NSView *_accessory;
    NSMutableArray<NSButton *> *_all;
    NSButton *_cancel;
    void (^_handler)(NSModalResponse);
    CGFloat _width;
    MDSheet *_retainedWhileShown;
}
- (instancetype)initWithTitle:(NSString *)title message:(NSString *)message width:(CGFloat)width {
    if (!(self = [super init])) return nil;
    _width = width; _all = [NSMutableArray array];
    MDSheetWindow *w = [[MDSheetWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, 120)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:YES];
    w.owner = self; w.releasedWhenClosed = NO; w.restorable = NO; w.title = title ?: @"";
    _window = w;
    NSTextField *heading = MDWrappingLabel(title);
    heading.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    heading.accessibilityRole = NSAccessibilityStaticTextRole;
    _messageLabel = MDWrappingLabel(message);
    _messageLabel.selectable = NO;
    _messageLabel.hidden = !message.length;
    _buttons = [NSStackView stackViewWithViews:@[]];
    _buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal; _buttons.spacing = 12;
    _stack = [NSStackView stackViewWithViews:@[heading, _messageLabel, _buttons]];
    _stack.orientation = NSUserInterfaceLayoutOrientationVertical; _stack.alignment = NSLayoutAttributeLeading;
    _stack.spacing = MDRowSpacing;
    [_stack setCustomSpacing:6 afterView:heading];
    [_stack setCustomSpacing:MDGroupSpacing afterView:_messageLabel];
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSView *v in @[heading, _messageLabel, _buttons]) [v.widthAnchor constraintEqualToAnchor:_stack.widthAnchor].active = YES;
    NSView *content = [NSView new];
    [content addSubview:_stack];
    [NSLayoutConstraint activateConstraints:@[
        [_stack.topAnchor constraintEqualToAnchor:content.topAnchor constant:MDMargin],
        [_stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:MDMargin],
        [_stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-MDMargin],
        [_stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-MDMargin],
        [_stack.widthAnchor constraintEqualToConstant:width - 2 * MDMargin]]];
    w.contentView = content;
    return self;
}
- (NSView *)accessory { return _accessory; }
- (void)setAccessory:(NSView *)accessory {
    if (_accessory) [_stack removeView:_accessory];
    _accessory = accessory;
    if (!accessory) return;
    [_stack insertArrangedSubview:accessory atIndex:2];
    [accessory.widthAnchor constraintEqualToAnchor:_stack.widthAnchor].active = YES;
    [_stack setCustomSpacing:MDGroupSpacing afterView:accessory];
    [_stack setCustomSpacing:12 afterView:_messageLabel];
}
- (NSButton *)addButton:(NSString *)title role:(MDSheetRole)role response:(NSModalResponse)response {
    NSButton *b = MDButton(title, self, @selector(choose:));
    b.tag = response;
    [b.widthAnchor constraintGreaterThanOrEqualToConstant:80].active = YES;
    switch (role) {
        case MDSheetDefault: b.keyEquivalent = @"\r"; [_buttons addView:b inGravity:NSStackViewGravityTrailing]; break;
        case MDSheetCancel: b.keyEquivalent = @"\e"; _cancel = b; [_buttons insertView:b atIndex:0 inGravity:NSStackViewGravityTrailing]; break;
        case MDSheetDestructive: b.hasDestructiveAction = YES; [_buttons addView:b inGravity:NSStackViewGravityLeading]; break;
        case MDSheetOther: [_buttons addView:b inGravity:NSStackViewGravityLeading]; break;
        case MDSheetAction: [_buttons addView:b inGravity:NSStackViewGravityTrailing]; break;
    }
    [_all addObject:b];
    return b;
}
- (void)makeCancelAlsoDefault { _cancel.keyEquivalent = @"\r"; }
- (NSButton *)buttonWithResponse:(NSModalResponse)response {
    for (NSButton *b in _all) if (b.tag == response) return b;
    return nil;
}
- (void)choose:(NSButton *)sender {
    if (!sender.enabled) return;
    NSWindow *parent = _window.sheetParent;
    if (parent) [parent endSheet:_window returnCode:sender.tag];
}
- (void)cancelSheet {
    if (_cancel.enabled) [self choose:_cancel];
}
- (void)beginSheetModalForWindow:(NSWindow *)parent completionHandler:(void (^)(NSModalResponse))handler {
    _handler = [handler copy];
    _window.initialFirstResponder = self.initialFirstResponder;
    [_window.contentView layoutSubtreeIfNeeded];
    [_window setContentSize:_window.contentView.fittingSize];
    _retainedWhileShown = self;
    [parent beginSheet:_window completionHandler:^(NSModalResponse response) {
        void (^done)(NSModalResponse) = self->_handler;
        self->_handler = nil; self->_retainedWhileShown = nil;
        [self->_window orderOut:nil];
        if (done) done(response);
    }];
    if (self.initialFirstResponder) [_window makeFirstResponder:self.initialFirstResponder];
}
@end
