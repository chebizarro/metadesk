#pragma once
#import <AppKit/AppKit.h>

/* One sidebar entry. Pages that are view controllers join the responder chain
 * while shown; plain views are hosted directly. */
@interface MDSection : NSObject
+ (instancetype)section:(NSString *)identifier symbol:(NSString *)symbol controller:(NSViewController *)controller view:(NSView *)view;
@property(nonatomic, readonly) NSString *identifier, *symbol;
@property(nonatomic, readonly) NSViewController *controller;
@property(nonatomic, readonly) NSView *view;
/* Short status shown at the trailing edge of the sidebar row (for example a
 * pending-request count); nil hides it. */
@property(nonatomic, copy) NSString *badge;
/* Refresh toolbar item tooltip for this section; nil omits the item. */
@property(nonatomic, copy) NSString *refreshHelp;
@property(nonatomic) BOOL searchable;
@end

/* Sidebar navigation, unified toolbar and a remembered window frame. Window
 * content is never restored: restorable is NO and only the frame is saved. */
@interface MDMainWindowController : NSWindowController <NSToolbarDelegate, NSWindowDelegate>
- (instancetype)initWithSections:(NSArray<MDSection *> *)sections;
@property(class, nonatomic, readonly) NSSize defaultContentSize, minimumContentSize;
@property(nonatomic, readonly) NSString *currentSection;
@property(nonatomic, readonly) NSArray<MDSection *> *sections;
@property(nonatomic, copy) void (^sectionChanged)(NSString *identifier);
/* Live search text for searchable sections. */
@property(nonatomic, copy) void (^searchChanged)(NSString *text);
@property(nonatomic, readonly) NSSearchField *searchField;
- (void)showSection:(NSString *)identifier;
- (void)focusSearch;
- (void)setBadge:(NSString *)badge forSection:(NSString *)identifier;
@end
