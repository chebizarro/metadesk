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
NSButton *MDButton(NSString *title, id target, SEL action);
NSButton *MDSymbolButton(NSString *symbol, NSString *accessibilityLabel, id target, SEL action);
NSStackView *MDRow(NSArray<NSView *> *views);
NSStackView *MDColumn(NSArray<NSView *> *views, CGFloat spacing);
/* Label/control grid: labels trail-aligned in the first column, rows aligned on
 * the first baseline. A row may be a single view that spans both columns; an
 * NSNull label leaves the label cell empty. */
NSGridView *MDForm(NSArray<NSArray *> *rows);
NSBox *MDSeparator(void);
/* Page container: a vertical stack pinned to the standard margins. */
NSView *MDPage(NSArray<NSView *> *views, NSStackView **stack);
/* A bordered scroll view around a table, with an overlay for its empty state. */
NSScrollView *MDTableScroll(NSTableView *table);
NSView *MDEmptyOverlay(NSScrollView *scroll, NSTextField **title, NSTextField **detail);
NSTableColumn *MDColumnNamed(NSTableView *table, NSString *identifier, NSString *title, CGFloat width, NSString *sortKey);
/* A reusable single-label table cell. */
NSTableCellView *MDCell(NSTableView *table, NSString *identifier, NSString *text);

typedef NS_ENUM(NSInteger, MDStatusKind) { MDStatusNone, MDStatusInfo, MDStatusWorking, MDStatusOK, MDStatusWarning, MDStatusError };
/* Icon or spinner followed by a wrapping message. */
@interface MDStatusView : NSStackView
- (void)setText:(NSString *)text kind:(MDStatusKind)kind;
@property(nonatomic, readonly) NSTextField *label;
@property(nonatomic, readonly) MDStatusKind kind;
@end

/* Confirmation alerts. MDAlertConfirm adds the action as the default (Return)
 * and Cancel (Escape). MDAlertDestructive adds Cancel (Escape) and a
 * destructive action that is never the default: Return does nothing. */
NSAlert *MDAlert(NSString *title, NSString *info);
void MDAlertConfirm(NSAlert *alert, NSString *action);
NSButton *MDAlertDestructive(NSAlert *alert, NSString *action);
/* Labeled, selectable details: @[@[@"Label", value], ...]. Values are plain text. */
NSGridView *MDDetails(NSArray<NSArray<NSString *> *> *rows);

/* Review and form sheets with explicit button roles. Buttons are laid out the
 * HIG way: other actions on the left, then Cancel, then the default action on
 * the right. Return activates the default button, if any; Escape and
 * cancelOperation: activate the cancel button, if any. A destructive button
 * gets no key equivalent. Each button ends the sheet with its response code. */
typedef NS_ENUM(NSInteger, MDSheetRole) { MDSheetDefault, MDSheetCancel, MDSheetOther, MDSheetDestructive };
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
