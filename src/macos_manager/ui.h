#pragma once
#import <AppKit/AppKit.h>

/* Shared AppKit building blocks for the Manager pages (md-b9zo). Everything is
 * Auto Layout with system fonts, semantic colors and the standard spacing
 * AppKit uses for its own windows; nothing sets a frame or a fixed color. */

enum {
    MDMargin = 20,      /* window and sheet content margin */
    MDGroupSpacing = 20, /* between unrelated groups */
    MDItemSpacing = 8,  /* between related controls and from a label to its control */
    MDRowSpacing = 10,  /* between form rows */
};

/* A single-line label that truncates instead of forcing the window wider. */
NSTextField *MDLabel(NSString *text);
/* Multi-line body text. */
NSTextField *MDWrappingLabel(NSString *text);
/* Secondary, explanatory text in the small system font. Wraps. */
NSTextField *MDNote(NSString *text);
/* Group heading: the system font in bold. */
NSTextField *MDHeader(NSString *text);
/* A trailing-aligned form label; pass the text without its colon. */
NSTextField *MDFormLabel(NSString *text);
/* A read-only value the user can select and copy. Remote and file-supplied
 * strings are shown as plain text; callers bound their length. */
NSTextField *MDValue(NSString *text);
NSTextField *MDWrappingValue(NSString *text);
/* An editable field for keys, names, URLs and paths: standard editing, undo and
 * contextual menu, with completion, correction and substitutions off. */
NSTextField *MDKeyField(NSString *placeholder, NSString *accessibilityLabel);
/* The field editor behind every MDKeyField: literal input that refuses
 * secret-key text before it reaches storage or undo (see ui.m). */
@interface MDKeyEditor : NSTextView
@property(nonatomic) BOOL typedOnly;
@end
/* Posted (object: the field, or the editor) when secret-key input is refused. */
extern NSNotificationName const MDSecretRefusedNotification;
BOOL MDContainsSecretKey(NSString *text);
/* Paste, drop and Services insertion are refused: the user must type. */
void MDSetTypedOnly(NSTextField *field);
/* Removes substitution, spelling and case commands (and, if typedOnly, paste). */
NSMenu *MDLiteralMenu(NSMenu *menu, BOOL typedOnly);
/* Test seam: the pasteboard Cut, Copy and Paste use in key fields. nil means
 * the general pasteboard. */
void MDSetEditingPasteboard(NSPasteboard *pasteboard);
NSButton *MDButton(NSString *title, id target, SEL action);
NSButton *MDSymbolButton(NSString *symbol, NSString *accessibilityLabel, id target, SEL action);
NSStackView *MDRow(NSArray<NSView *> *views);
NSStackView *MDColumn(NSArray<NSView *> *views, CGFloat spacing);
/* Label/control grid: labels trail-aligned in the first column, rows aligned on
 * the first baseline. A row may be a single view that spans both columns; an
 * NSNull label leaves the label cell empty. */
NSGridView *MDForm(NSArray<NSArray *> *rows);
/* Gives several forms one label-column width so their controls line up. */
void MDAlignForms(NSArray<NSGridView *> *forms);
/* Hides a view and, inside a form, its whole row so no gap is left. */
void MDSetRowHidden(NSView *view, BOOL hidden);
NSBox *MDSeparator(void);
/* Page container: a vertical stack pinned to the standard margins. */
NSView *MDPage(NSArray<NSView *> *views, NSStackView **stack);
/* Lets a form page scroll vertically when the window is shorter than it. */
NSView *MDScrollingPage(NSView *page);
/* A bordered scroll view around a table, with an overlay for its empty state. */
NSScrollView *MDTableScroll(NSTableView *table);
/* Wraps the scroll view in a container with a centered empty-state overlay.
 * Returns the container (which takes the remaining height in an MDPage). */
NSView *MDTableArea(NSScrollView *scroll, NSView **overlay, NSTextField **title, NSTextField **detail);
NSTableColumn *MDColumnNamed(NSTableView *table, NSString *identifier, NSString *title, CGFloat width, NSString *sortKey);
/* A table whose named flexible column alone absorbs width changes. Column
 * autoresizing starts only once the table has a real width, so columns are
 * not squeezed to their minimum while the view is still being assembled. */
@interface MDTable : NSTableView
@end
/* After adding columns: only the named column absorbs width changes. */
void MDFinishColumns(NSTableView *table, NSString *flexible);
/* A reusable single-label table cell. */
NSTableCellView *MDCell(NSTableView *table, NSString *identifier, NSString *text);

typedef NS_ENUM(NSInteger, MDStatusKind) { MDStatusNone, MDStatusInfo, MDStatusWorking, MDStatusOK, MDStatusWarning, MDStatusError };
/* Icon or spinner followed by a wrapping message. */
@interface MDStatusView : NSStackView
- (void)setText:(NSString *)text kind:(MDStatusKind)kind;
@property(nonatomic, readonly) NSTextField *label;
@property(nonatomic, readonly) MDStatusKind kind;
@end

/* A rounded callout shown above content: status icon, bold title, a sentence
 * of explanation and the buttons for the next step. A nil title hides it. */
@interface MDCallout : NSView
- (void)showTitle:(NSString *)title text:(NSString *)text kind:(MDStatusKind)kind buttons:(NSArray<NSButton *> *)buttons;
@property(nonatomic, readonly) NSTextField *titleLabel, *textLabel;
@end
/* A centered first-run panel that replaces a page whose prerequisites are
 * missing: a large symbol, title, explanation, one prominent next step and an
 * optional secondary button. */
@interface MDSetupPanel : NSView
- (void)showSymbol:(NSString *)symbol title:(NSString *)title text:(NSString *)text
           primary:(NSButton *)primary secondary:(NSButton *)secondary working:(BOOL)working;
@property(nonatomic, readonly) NSTextField *titleLabel, *textLabel;
@property(nonatomic, readonly) NSButton *primary, *secondary;
@end
/* A prominent (accent-tinted) push button for the one next step. */
NSButton *MDProminentButton(NSString *title, id target, SEL action);

/* Confirmation alerts. MDAlertConfirm adds the action as the default (Return)
 * and Cancel (Escape). MDAlertDestructive adds Cancel (Escape) and a
 * destructive action that is never the default: Return does nothing. */
NSAlert *MDAlert(NSString *title, NSString *info);
void MDAlertConfirm(NSAlert *alert, NSString *action);
NSButton *MDAlertDestructive(NSAlert *alert, NSString *action);
/* Labeled, selectable details: @[@[@"Label", value], ...]. Values are plain text. */
NSGridView *MDDetails(NSArray<NSArray<NSString *> *> *rows);
/* A selectable key or fingerprint in a monospaced font that wraps by character. */
NSTextField *MDKeyValue(NSString *text);

/* Review and form sheets with explicit button roles. Buttons are laid out the
 * HIG way: other actions on the left, then Cancel, then the default action on
 * the right. Return activates the default button, if any; Escape and
 * cancelOperation: activate the cancel button, if any. A destructive button
 * gets no key equivalent. Each button ends the sheet with its response code. */
/* MDSheetAction sits with Cancel on the right but has no key equivalent: use
 * it for trust decisions, which must be chosen deliberately, never by Return. */
typedef NS_ENUM(NSInteger, MDSheetRole) { MDSheetDefault, MDSheetCancel, MDSheetOther, MDSheetDestructive, MDSheetAction };
@interface MDSheet : NSObject
- (instancetype)initWithTitle:(NSString *)title message:(NSString *)message width:(CGFloat)width;
@property(nonatomic, readonly) NSWindow *window;
@property(nonatomic, readonly) NSTextField *messageLabel;
@property(nonatomic) NSView *accessory;
@property(nonatomic, weak) NSView *initialFirstResponder;
/* The cancel role may also be the default; Return and Escape then both choose it. */
- (NSButton *)addButton:(NSString *)title role:(MDSheetRole)role response:(NSModalResponse)response;
- (void)makeCancelAlsoDefault;
- (void)beginSheetModalForWindow:(NSWindow *)parent completionHandler:(void (^)(NSModalResponse))handler;
- (NSButton *)buttonWithResponse:(NSModalResponse)response;
@end
