#import "manager_app.h"
#import "hosts_page.h"
#import "hosts_model.h"
#import "access_page.h"
#import "identity_page.h"
#import "main_window.h"
#import "ui.h"
#include "md_test.h"
#import <UserNotifications/UserNotifications.h>
#include <stdatomic.h>
#include <unistd.h>

#define CHECK(x) do { MD_CHECK(x); if (md_test_failures) return NO; } while (0)
#define KEY "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define OTHER "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9"
#define GEN "abababababababababababababababab"
#define REQUEST "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
#define REQUEST2 "efefefefefefefefefefefefefefefef"
#define THIRD "e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd13"

/* Drive the main run loop on dispatched callbacks and sheet operations. The
 * deadline is a watchdog, not a delay used to make an assertion pass. */
static BOOL Await(BOOL (^ready)(void)) {
    CFAbsoluteTime limit=CFAbsoluteTimeGetCurrent()+3;
    while (!ready()) {
        CFTimeInterval left=limit-CFAbsoluteTimeGetCurrent();
        if (left<=0) return NO;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,left,true);
    }
    return YES;
}
static NSView *Find(NSView *root, BOOL (^matches)(NSView *)) {
    if (root && matches(root)) return root;
    for (NSView *view in root.subviews) {
        NSView *hit=Find(view,matches);
        if (hit) return hit;
    }
    return nil;
}
/* Matches a button's title, or the accessibility label of an image button. */
static NSButton *Button(NSView *view, NSString *title) {
    return (NSButton *)Find(view,^BOOL(NSView *v) {
        return [v isKindOfClass:NSButton.class] && ([((NSButton *)v).title isEqual:title] ||
            (!((NSButton *)v).title.length && [v.accessibilityLabel isEqual:title]));
    });
}
/* Matches an editable field by placeholder or accessibility label prefix. */
static NSTextField *Field(NSView *view, NSString *prefix) {
    return (NSTextField *)Find(view,^BOOL(NSView *v) {
        return [v isKindOfClass:NSTextField.class] && ((NSTextField *)v).editable &&
            ([((NSTextField *)v).placeholderString hasPrefix:prefix] || [v.accessibilityLabel hasPrefix:prefix]);
    });
}
static BOOL LabelContains(NSView *view, NSString *message) {
    return Find(view,^BOOL(NSView *v) {
        return [v isKindOfClass:NSTextField.class] && [((NSTextField *)v).stringValue containsString:message];
    }) != nil;
}
/* Trust decisions must be chosen deliberately: no button answers Return. */
static BOOL NoDefault(NSView *sheet) {
    return Find(sheet,^BOOL(NSView *v) {
        return [v isKindOfClass:NSButton.class] && [((NSButton *)v).keyEquivalent isEqual:@"\r"];
    }) == nil;
}
static BOOL Escapes(NSButton *button) { return [button.keyEquivalent isEqual:@"\e"]; }
static void Typed(NSTextField *field, NSString *text) {
    field.stringValue = text;
    [NSNotificationCenter.defaultCenter postNotificationName:NSControlTextDidChangeNotification object:field];
}
static BOOL Send(NSControl *control) {
    return control.enabled && [NSApp sendAction:control.action to:control.target from:control];
}
static BOOL EndSheet(NSWindow *window, NSModalResponse response) {
    NSWindow *sheet=window.attachedSheet;
    if (!sheet) return NO;
    [window endSheet:sheet returnCode:response];
    return Await(^BOOL{ return window.attachedSheet==nil; });
}

/* The menu bar must carry the standard commands with nil targets, so they
 * reach the field editor, window or app delegate through the responder chain. */
static NSMenuItem *MenuItem(NSMenu *menu, SEL action) {
    for (NSMenuItem *item in menu.itemArray) {
        if (item.action == action) return item;
        NSMenuItem *hit = item.submenu ? MenuItem(item.submenu, action) : nil;
        if (hit) return hit;
    }
    return nil;
}
static NSMenu *TopMenu(NSMenu *bar, NSString *title) {
    for (NSMenuItem *item in bar.itemArray) if ([item.submenu.title isEqual:title]) return item.submenu;
    return nil;
}
static BOOL Menus(void) {
    NSMenu *bar = [MDManagerApp makeMainMenu];
    const NSEventModifierFlags C = NSEventModifierFlagCommand, S = NSEventModifierFlagShift,
        O = NSEventModifierFlagOption, X = NSEventModifierFlagControl;
    struct { const char *menu, *title, *key; SEL action; NSEventModifierFlags mask; } want[] = {
        {"Metadesk Manager", "About Metadesk Manager", "", @selector(orderFrontStandardAboutPanel:), 0},
        {"Metadesk Manager", "Hide Metadesk Manager", "h", @selector(hide:), C},
        {"Metadesk Manager", "Hide Others", "h", @selector(hideOtherApplications:), C | O},
        {"Metadesk Manager", "Show All", "", @selector(unhideAllApplications:), 0},
        {"Metadesk Manager", "Quit Metadesk Manager", "q", @selector(terminate:), C},
        {"File", "Connect to Host…", "k", @selector(connectToHost:), C},
        {"File", "Scan QR Code…", "", @selector(scanQR:), 0},
        {"File", "Import Hosts…", "", @selector(importHosts:), 0},
        {"File", "Export Hosts…", "", @selector(exportHosts:), 0},
        {"File", "Close Window", "w", @selector(performClose:), C},
        {"Edit", "Undo", "z", @selector(undo:), C},
        {"Edit", "Redo", "z", @selector(redo:), C | S},
        {"Edit", "Cut", "x", @selector(cut:), C},
        {"Edit", "Copy", "c", @selector(copy:), C},
        {"Edit", "Paste", "v", @selector(paste:), C},
        {"Edit", "Delete", "", @selector(delete:), 0},
        {"Edit", "Select All", "a", @selector(selectAll:), C},
        {"Edit", "Find Hosts…", "f", @selector(findHosts:), C},
        {"View", "Refresh", "r", @selector(refresh:), C},
        {"View", "Show Sidebar", "s", @selector(toggleSidebar:), C | X},
        {"View", "Enter Full Screen", "f", @selector(toggleFullScreen:), C | X},
        {"Window", "Minimize", "m", @selector(performMiniaturize:), C},
        {"Window", "Zoom", "", @selector(performZoom:), 0},
        {"Window", "Bring All to Front", "", @selector(arrangeInFront:), 0},
        {"Help", "Metadesk Manager Help", "?", @selector(openHelp:), C},
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(*want); ++i) {
        NSMenu *menu = TopMenu(bar, @(want[i].menu));
        NSMenuItem *item = MenuItem(menu, want[i].action);
        if (!item) fprintf(stderr, "missing menu item %s > %s\n", want[i].menu, want[i].title);
        CHECK(item && [item.title isEqual:@(want[i].title)] && item.target == nil);
        CHECK([item.keyEquivalent isEqual:@(want[i].key)]);
        CHECK(!item.keyEquivalent.length || item.keyEquivalentModifierMask == want[i].mask);
    }
    NSArray *order = [bar.itemArray valueForKeyPath:@"submenu.title"];
    CHECK([order isEqual:(@[@"Metadesk Manager", @"File", @"Edit", @"View", @"Window", @"Help"])]);
    NSMenu *view = TopMenu(bar, @"View");
    NSArray *sections = MDManagerApp.sectionTitles;
    for (NSUInteger i = 0; i < sections.count; ++i) {
        NSMenuItem *item = view.itemArray[i];
        CHECK(item.action == @selector(showSection:) && [item.representedObject isEqual:sections[i]] && [item.title isEqual:sections[i]]);
        CHECK([item.keyEquivalent isEqual:([NSString stringWithFormat:@"%lu", (unsigned long)i + 1])]);
    }
    /* Key equivalents are unique per modifier set. */
    NSMutableSet *keys = [NSMutableSet set];
    NSMutableArray *queue = [bar.itemArray mutableCopy];
    while (queue.count) {
        NSMenuItem *item = queue.lastObject; [queue removeLastObject];
        if (item.submenu) [queue addObjectsFromArray:item.submenu.itemArray];
        if (!item.keyEquivalent.length) continue;
        NSString *k = [NSString stringWithFormat:@"%@/%lu", item.keyEquivalent, (unsigned long)item.keyEquivalentModifierMask];
        CHECK(![keys containsObject:k]); [keys addObject:k];
    }
    [MDManagerApp installMainMenu:NSApp];
    CHECK(NSApp.windowsMenu == TopMenu(bar, @"Window") || [NSApp.windowsMenu.title isEqual:@"Window"]);
    CHECK([NSApp.helpMenu.title isEqual:@"Help"] && [NSApp.servicesMenu.title isEqual:@"Services"]);
    return YES;
}

/* Commands are dispatched exactly as AppKit does for the menu bar: validate
 * with -[NSMenu update] and send with a nil target through NSApp, which
 * resolves the key window's responder chain. Test windows are never ordered
 * on screen, so the test application names the key window explicitly. */
static NSWindow *KeyWindowOverride;
@interface TestApplication : NSApplication
@end
@implementation TestApplication
- (NSWindow *)keyWindow { return KeyWindowOverride ?: [super keyWindow]; }
- (NSWindow *)mainWindow { return KeyWindowOverride ?: [super mainWindow]; }
@end
static void Turn(void) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false); }
static BOOL Enabled(NSWindow *key, SEL action) {
    KeyWindowOverride = key;
    NSMenuItem *item = MenuItem(NSApp.mainMenu, action);
    [item.menu update];
    return item && item.isEnabled;
}
static BOOL Choose(NSWindow *key, SEL action) {
    if (!Enabled(key, action)) return NO;
    NSMenuItem *item = MenuItem(NSApp.mainMenu, action);
    return [NSApp sendAction:item.action to:item.target from:item];
}
/* Independent of the code under test: any trace of a secret-key prefix. */
static BOOL HasSecret(NSString *text) {
    NSString *lower = text.lowercaseString;
    return [lower containsString:@"nsec1"] || [lower containsString:@"ncryptsec1"];
}
static BOOL Visible(NSView *view) { return view && view.window && !view.isHiddenOrHasHiddenAncestor; }
static NSView *SetupPanel(NSView *root) { return Find(root,^BOOL(NSView *v){ return [v isKindOfClass:MDSetupPanel.class]; }); }
static BOOL MenuHasNamed(NSMenu *menu, NSString *prefix) {
    for (NSMenuItem *item in menu.itemArray)
        if ((item.action && [NSStringFromSelector(item.action) hasPrefix:prefix]) || (item.submenu && MenuHasNamed(item.submenu, prefix))) return YES;
    return NO;
}
/* A drop driven through NSTextView's own drag-destination methods. It is a
 * plain object passed as an argument; no event is posted. */
@interface FixtureDrop : NSObject <NSDraggingInfo>
@property(strong) NSPasteboard *pasteboard;
@property(weak) NSWindow *window;
@property NSPoint location;
@end
@implementation FixtureDrop
@synthesize draggingFormation, animatesToDestination, numberOfValidItemsForDrop, springLoadingHighlight;
- (NSWindow *)draggingDestinationWindow { return self.window; }
- (NSDragOperation)draggingSourceOperationMask { return NSDragOperationCopy | NSDragOperationGeneric; }
- (NSPoint)draggingLocation { return self.location; }
- (NSPoint)draggedImageLocation { return self.location; }
- (NSImage *)draggedImage { return nil; }
- (NSPasteboard *)draggingPasteboard { return self.pasteboard; }
- (id)draggingSource { return nil; }
- (NSInteger)draggingSequenceNumber { return 1; }
- (void)slideDraggedImageTo:(NSPoint)point { (void)point; }
- (NSArray<NSString *> *)namesOfPromisedFilesDroppedAtDestination:(NSURL *)url { (void)url; return nil; }
- (void)enumerateDraggingItemsWithOptions:(NSDraggingItemEnumerationOptions)o forView:(NSView *)v classes:(NSArray<Class> *)c
    searchOptions:(NSDictionary<NSPasteboardReadingOptionKey, id> *)so usingBlock:(void (^)(NSDraggingItem *, NSInteger, BOOL *))b {
    (void)o; (void)v; (void)c; (void)so; (void)b;
}
- (void)resetSpringLoading {}
@end
static BOOL Drop(NSTextView *editor, NSPasteboard *pb) {
    FixtureDrop *drop = [FixtureDrop new]; drop.pasteboard = pb; drop.window = editor.window;
    drop.location = [editor convertPoint:NSMakePoint(NSMaxX(editor.bounds) - 1, NSMidY(editor.bounds)) toView:nil];
    if (!([editor draggingEntered:drop] & NSDragOperationCopy)) return NO;
    BOOL accepted = [editor prepareForDragOperation:drop] && [editor performDragOperation:drop];
    [editor concludeDragOperation:drop];
    return accepted;
}
static BOOL MenuHas(NSMenu *menu, SEL action) {
    for (NSMenuItem *item in menu.itemArray) if (item.action == action || (item.submenu && MenuHas(item.submenu, action))) return YES;
    return NO;
}

/* Edit menu commands reach the connect field's editor through the real
 * responder chain of the real main window (S10). */
static BOOL Editing(MDManagerApp *app, NSWindow *window, MDHostsPage *page) {
    [app showSection:@"Hosts"];
    NSTextField *address = Field(page.view, @"Host npub");
    CHECK(Await(^BOOL{ return Visible(address); }));
    CHECK(address && [window makeFirstResponder:address]);
    MDKeyEditor *editor = (MDKeyEditor *)address.currentEditor;
    CHECK([editor isKindOfClass:MDKeyEditor.class] && editor.allowsUndo && window.firstResponder == editor);
    NSPasteboard *pb = [NSPasteboard pasteboardWithUniqueName];
    MDSetEditingPasteboard(pb);
    NSString *npub = MDHostNpub(@KEY);
    editor.string = @""; [window.undoManager removeAllActions]; Turn();
    [pb clearContents]; CHECK([pb setString:npub forType:NSPasteboardTypeString]);
    CHECK(Choose(window, @selector(paste:)) && [address.stringValue isEqual:npub]);
    Turn();
    CHECK(Choose(window, @selector(selectAll:)) && editor.selectedRange.length == npub.length);
    [pb clearContents];
    CHECK(Choose(window, @selector(copy:)) && [[pb stringForType:NSPasteboardTypeString] isEqual:npub]);
    [pb clearContents];
    CHECK(Choose(window, @selector(cut:)) && !address.stringValue.length && [[pb stringForType:NSPasteboardTypeString] isEqual:npub]);
    Turn();
    CHECK(Choose(window, @selector(undo:)) && [address.stringValue isEqual:npub]);
    Turn();
    CHECK(Choose(window, @selector(redo:)) && !address.stringValue.length);
    Turn();
    /* Literal input (S7): no smart spaces, substitutions stay off and are not
     * offered by the contextual menu. */
    CHECK(!editor.smartInsertDeleteEnabled);
    [editor insertText:@"npub" replacementRange:editor.selectedRange];
    [pb clearContents]; CHECK([pb setString:@"1abc" forType:NSPasteboardTypeString]);
    CHECK(Choose(window, @selector(paste:)) && [editor.string isEqual:@"npub1abc"]);
    SEL toggles[] = {@selector(toggleAutomaticQuoteSubstitution:), @selector(toggleSmartInsertDelete:), @selector(toggleAutomaticTextReplacement:),
                     @selector(toggleAutomaticSpellingCorrection:), @selector(uppercaseWord:)};
    for (size_t i = 0; i < sizeof toggles / sizeof *toggles; ++i) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"" action:toggles[i] keyEquivalent:@""];
        CHECK(![editor validateMenuItem:item]);
        [NSApp sendAction:toggles[i] to:editor from:item];
    }
    CHECK(!editor.automaticQuoteSubstitutionEnabled && !editor.smartInsertDeleteEnabled && !editor.automaticTextReplacementEnabled &&
          [editor.string isEqual:@"npub1abc"]);
    NSMenu *context = MDLiteralMenu([NSTextView.defaultMenu copy], NO);
    for (size_t i = 0; i < sizeof toggles / sizeof *toggles; ++i) CHECK(!MenuHas(context, toggles[i]));
    CHECK(MenuHas(context, @selector(paste:)) && MenuHas(context, @selector(copy:)));
    /* An event object built only as an argument; it is never posted. */
    NSEvent *click = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown location:NSZeroPoint modifierFlags:0 timestamp:0
        windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
    editor.string = @"npub1abc"; [editor setSelectedRange:NSMakeRange(0, 4)];
    NSMenu *ownMenu = click ? [editor menuForEvent:click] : nil;
    CHECK(ownMenu != nil);
    /* P3: Look Up, Translate, Search, Share and Writing Tools are gone... */
    for (NSString *name in @[@"_rvMenuItem", @"_searchWith", @"_performStandardShare", @"showWritingTools", @"replaceQuotesInSelection"])
        CHECK(!MenuHasNamed(ownMenu, name));
    /* ...while a stock field offers Look Up, so the check is meaningful. */
    NSTextField *stock = [NSTextField textFieldWithString:@"npub1abc"];
    [window.contentView addSubview:stock];
    CHECK([window makeFirstResponder:stock]);
    NSTextView *stockEditor = (NSTextView *)stock.currentEditor;
    [stockEditor setSelectedRange:NSMakeRange(0, 4)];
    CHECK(MenuHasNamed([stockEditor menuForEvent:click], @"_rvMenuItem"));
    [window makeFirstResponder:nil]; [stock removeFromSuperview];
    CHECK([window makeFirstResponder:address]);
    editor = (MDKeyEditor *)address.currentEditor;
    for (size_t i = 0; ownMenu && i < sizeof toggles / sizeof *toggles; ++i) CHECK(!MenuHas(ownMenu, toggles[i]));
    editor.string = @""; [window.undoManager removeAllActions];
    [pb releaseGlobally]; MDSetEditingPasteboard(nil);
    CHECK([window makeFirstResponder:nil]);
    return YES;
}

/* Secret-key text is refused before it reaches the field's storage or undo
 * stack, whichever route inserts it: typing, Paste, drop, Services; and Undo
 * and Redo afterwards never bring it back (B2). */
static BOOL Refuses(NSWindow *key, NSTextField *field, NSView *messageView, NSString *message) {
    KeyWindowOverride = key;
    CHECK(field && [key makeFirstResponder:field]);
    MDKeyEditor *editor = (MDKeyEditor *)field.currentEditor;
    CHECK([editor isKindOfClass:MDKeyEditor.class]);
    __block BOOL stored = NO;
    id watch = [NSNotificationCenter.defaultCenter addObserverForName:NSTextStorageDidProcessEditingNotification object:editor.textStorage
        queue:nil usingBlock:^(NSNotification *note) { if (HasSecret(((NSTextStorage *)note.object).string)) stored = YES; }];
    NSPasteboard *pb = [NSPasteboard pasteboardWithUniqueName];
    MDSetEditingPasteboard(pb);
    /* Setting the string directly bypasses undo; start from a clean stack. */
    editor.string = @""; [key.undoManager removeAllActions]; Turn();
    /* The drop path is live: a legitimate drop is inserted. */
    if (!editor.typedOnly) {
        [pb clearContents]; [pb setString:@"xyz" forType:NSPasteboardTypeString];
        CHECK(Drop(editor, pb) && [editor.string containsString:@"xyz"]);
        Turn(); /* close the drop's undo group before clearing the stack */
        editor.string = @""; [key.undoManager removeAllActions]; Turn();
    }
    [editor insertText:@"abc" replacementRange:editor.selectedRange]; Turn();
    for (NSString *secret in @[@"nsec1fixturenotakey", @"  nostr:nsec1fixture", @"NSEC1FIXTURE", @"ncryptsec1fixture", @"npub1x nsec1fixture"]) {
        [editor insertText:secret replacementRange:editor.selectedRange];                 /* typing */
        [pb clearContents]; [pb setString:secret forType:NSPasteboardTypeString];
        Choose(key, @selector(paste:));                                                    /* Edit > Paste */
        Drop(editor, pb);                                                                   /* drag and drop */
        /* Services: readSelectionFromPasteboard: is the NSServicesMenuRequestor
         * method AppKit calls to insert a Service's result. */
        [editor readSelectionFromPasteboard:pb];
        Turn();
        CHECK(!HasSecret(editor.string) && !HasSecret(field.stringValue));
    }
    CHECK(!stored);
    if (message) CHECK(LabelContains(messageView, message));
    /* Undo and Redo both run, and neither restores the refused text. */
    /* Undo walks back to before the legitimate "abc" and Redo forward again;
     * refused insertions may leave empty undo groups, but no step ever shows
     * the refused text. */
    BOOL undone = NO, redone = NO;
    for (int i = 0; i < 8 && !undone; ++i) {
        CHECK(Choose(key, @selector(undo:))); Turn();
        CHECK(!HasSecret(editor.string) && !stored);
        undone = editor.string.length == 0;
    }
    CHECK(undone);
    for (int i = 0; i < 8 && !redone; ++i) {
        CHECK(Choose(key, @selector(redo:))); Turn();
        CHECK(!HasSecret(editor.string) && !stored);
        redone = [editor.string isEqual:@"abc"];
    }
    CHECK(redone);
    while (Enabled(key, @selector(redo:))) { Choose(key, @selector(redo:)); Turn(); CHECK(!HasSecret(editor.string) && !stored); }
    [NSNotificationCenter.defaultCenter removeObserver:watch];
    editor.string = @""; [key.undoManager removeAllActions];
    [pb releaseGlobally]; MDSetEditingPasteboard(nil);
    [key makeFirstResponder:nil];
    return YES;
}

@interface FixtureReview : MDHostReview
@property BOOL mismatch;
@end
@implementation FixtureReview
- (NSString *)key { return @KEY; }
- (NSString *)npub { return MDHostNpub(self.key); }
- (NSString *)fingerprint { return MDHostFingerprint(self.key); }
- (NSString *)oldKey { return self.mismatch ? @OTHER : nil; }
- (NSString *)alias { return self.mismatch ? @"desk@example.invalid" : nil; }
- (uint8_t)caps { return MD_CAP_VIDEO; }
@end
@interface FixtureBook : MDHostsBook
@property MDHostsState fixtureState;
@property(strong) NSString *fixtureOwner;
@end
@implementation FixtureBook
- (NSString *)owner { return self.fixtureOwner; }
/* Model fixture: a usable identity/cache is independent of sync. */
- (MDHostsState)state { return self.fixtureState; }
- (BOOL)canReview { return self.fixtureState == MDHostsSynced; }
- (BOOL)canReviewKey { return self.fixtureState >= MDHostsNoRelays && self.fixtureState != MDHostsDocumentError; }
- (BOOL)canEdit { return self.fixtureState == MDHostsSynced; }
- (BOOL)canReplaceReview:(MDHostReview *)review { return review.oldKey!=nil; }
- (BOOL)canConfirmReview:(MDHostReview *)review fingerprint:(NSString *)fingerprint {
    return [self canReplaceReview:review] && [review.fingerprint isEqual:fingerprint];
}
@end
@interface FixtureHosts : MDHostsWorker
@property NSUInteger resolves,saves,launches,closes,starts;
@property(strong) NSString *relays,*owner;
@property BOOL mismatch,observeCancellation;
@property MDHostsState state;
@property(strong) NSString *input,*confirmation;
@property(strong) MDHostReview *savedReview,*launchReview;
@property(copy) dispatch_block_t entered;
@property(strong) dispatch_semaphore_t proceed;
@end
@implementation FixtureHosts
- (BOOL)openDirectory:(NSString *)directory error:(NSString **)error { (void)directory; (void)error; return YES; }
- (MDHostsBook *)snapshot { FixtureBook *b=[FixtureBook new]; b.fixtureState=self.state; b.fixtureOwner=self.owner; return b; }
- (BOOL)startRelays:(NSString *)urls { ++self.starts; self.relays=urls; return YES; }
- (MDHostReview *)resolve:(NSString *)value route:(NSString *)route cancelled:(bool (*)(void *))cancelled data:(void *)data error:(NSString **)error {
    (void)route; (void)error;
    ++self.resolves; self.input=value;
    if (self.entered) {
        dispatch_async(dispatch_get_main_queue(),self.entered);
        if (dispatch_semaphore_wait(self.proceed,dispatch_time(DISPATCH_TIME_NOW,4*NSEC_PER_SEC))) return nil;
        self.observeCancellation=cancelled(data);
    }
    FixtureReview *result=[FixtureReview new]; result.mismatch=self.mismatch; return result;
}
- (BOOL)launchConfig:(MdManagedLaunch *)config review:(MDHostReview *)review error:(NSString **)error {
    (void)config; self.launchReview=review; ++self.launches;
    *error=@"Fixture stopped before starting a child.";
    return NO;
}
- (NSString *)saveReview:(MDHostReview *)review confirmation:(NSString *)fingerprint {
    self.savedReview=review; self.confirmation=fingerprint; ++self.saves; return @"";
}
- (void)close { ++self.closes; }
@end
@interface MDHostsPage (FixtureScan)
- (void)reviewInput:(NSString *)input fromImage:(BOOL)fromImage;
@end

static BOOL Hosts(MDHostsPage *page, FixtureHosts *worker, NSWindow *window) {
    NSView *view=page.view;
    NSTextField *address=Field(view,@"Host npub");
    NSButton *review=Button(view,@"Review and Connect…");
    CHECK(address && review);
    /* P4: wait for the ready transition; the controls driven below are the
     * visible ones, not the content hidden behind the opening panel. */
    [page activate];
    CHECK(Await(^BOOL{ return review.enabled && Visible(address); }));
    CHECK(!Visible(SetupPanel(view)) && Visible(review));
    CHECK(Refuses(window, Field(view,@"Optional IPv4"), view, @"Private keys are not host addresses"));
    CHECK(Refuses(window, address, view, @"Private keys are not host addresses"));
    NSString *config=[@(getenv("XDG_CONFIG_HOME")) stringByAppendingPathComponent:@"metadesk"];
    CHECK([NSFileManager.defaultManager createDirectoryAtPath:config withIntermediateDirectories:YES
        attributes:@{NSFilePosixPermissions:@0700} error:NULL]);
    NSString *prefs=[config stringByAppendingPathComponent:@"manager-relays.ini"];
    NSString *corrupt=@"not a relay preferences file";
    CHECK([corrupt writeToFile:prefs atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    [page activate]; CHECK(Await(^BOOL{ return review.enabled; }));
    /* View > Refresh and the toolbar's Refresh reach the page as refresh:.
     * Refresh only syncs the saved list: an unreadable file is reported and
     * left alone, with no offer to overwrite it (that is Save's job). */
    NSMenuItem *refresh=MenuItem([MDManagerApp makeMainMenu],@selector(refresh:));
    BOOL (^canRefresh)(void)=^BOOL{ return [page validateMenuItem:refresh]; };
    NSUInteger starts=worker.starts;
    CHECK(canRefresh() && [NSApp sendAction:@selector(refresh:) to:page from:refresh]);
    CHECK(Await(^BOOL{ return canRefresh() && LabelContains(view,@"Could not read saved bootstrap"); }));
    CHECK(!window.attachedSheet && worker.starts==starts);
    CHECK([corrupt isEqual:[NSString stringWithContentsOfFile:prefs encoding:NSUTF8StringEncoding error:NULL]]);
    CHECK([@"[relays]\nbootstrap=wss://fixture.invalid/;\n" writeToFile:prefs atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    __block BOOL drained=NO;
    [page suspendForRoleEdit:^{ drained=YES; }];
    CHECK(Await(^BOOL{ return drained; }));
    [page resumeAfterRoleEdit];
    CHECK(Await(^BOOL{ return canRefresh() && worker.starts==starts+1; }));

    address.stringValue=@"  nostr:nsec1fixture-not-a-key";
    CHECK(Send(review));
    CHECK(!address.stringValue.length && LabelContains(view,@"Private keys are not host addresses"));
    address.stringValue=@KEY;
    [page reviewInput:@"ncryptsec1fixture-not-a-key" fromImage:YES];
    CHECK(!address.stringValue.length && !worker.resolves && !window.attachedSheet);

    [page reviewInput:@"desk@example.invalid" fromImage:YES];
    [page reviewInput:@KEY fromImage:YES];
    CHECK(!worker.resolves && LabelContains(view,@"doesn’t contain a host’s public nprofile"));
    address.stringValue=@KEY;
    CHECK(Send(review) && !review.enabled && !worker.saves && !worker.launches);
    CHECK(Await(^BOOL{ return window.attachedSheet!=nil; }));
    NSView *sheet=window.attachedSheet.contentView;
    CHECK(Button(sheet,@"Connect").enabled && Button(sheet,@"Save Host").enabled);
    CHECK(NoDefault(sheet) && Escapes(Button(sheet,@"Cancel")));
    CHECK([worker.input isEqual:@KEY] && worker.resolves==1 && !worker.saves && !worker.launches);
    CHECK(EndSheet(window,NSAlertFirstButtonReturn) && !worker.saves && !worker.launches);
    CHECK(Send(review) && Await(^BOOL{ return window.attachedSheet!=nil; }));
    CHECK(EndSheet(window,NSAlertThirdButtonReturn));
    CHECK(Await(^BOOL{ return LabelContains(view,@"Host saved."); }));
    CHECK(worker.saves==1 && [worker.savedReview.key isEqual:@KEY] && !worker.launches);
    CHECK(Send(review) && Await(^BOOL{ return window.attachedSheet!=nil; }));
    CHECK(EndSheet(window,NSAlertSecondButtonReturn));
    CHECK(Await(^BOOL{ return LabelContains(view,@"Fixture stopped before"); }));
    CHECK(worker.launches==1 && [worker.launchReview.key isEqual:@KEY]);

    for (NSNumber *state in @[@(MDHostsSyncing), @(MDHostsRelaysOffline), @(MDHostsRelayRefused)]) {
        worker.state=state.intValue;
        CHECK([NSApp sendAction:@selector(refresh:) to:page from:refresh]);
        CHECK(Await(^BOOL{ return review.enabled && Visible(address) && !Visible(SetupPanel(view)) && LabelContains(view,@"To connect by name"); }));
        NSUInteger resolves=worker.resolves, launches=worker.launches, saves=worker.saves;
        address.stringValue=@"desk@example.invalid";
        CHECK(Send(review) && LabelContains(view,@"before reviewing a NIP-05 name"));
        CHECK(worker.resolves==resolves && !window.attachedSheet);
        address.stringValue=@KEY;
        CHECK(Send(review) && Await(^BOOL{ return window.attachedSheet!=nil; }));
        sheet=window.attachedSheet.contentView;
        CHECK(Button(sheet,@"Connect").enabled && Button(sheet,@"Save Host") && !Button(sheet,@"Save Host").enabled);
        CHECK(LabelContains(sheet,@"Saving requires a synced, writable host list") && LabelContains(sheet,@"Fingerprint"));
        CHECK(worker.launches==launches && worker.saves==saves);
        CHECK(EndSheet(window,NSAlertFirstButtonReturn));
    }
    worker.state=MDHostsDocumentError;
    CHECK([NSApp sendAction:@selector(refresh:) to:page from:refresh]);
    CHECK(Await(^BOOL{ return canRefresh() && Visible(address) && !Visible(SetupPanel(view)) &&
        LabelContains(view,@"Part of Your Host List Can’t Be Read"); }));
    CHECK(!review.enabled && !Button(view,@"Scan QR Code…").enabled &&
        !LabelContains(view,@"use the host’s public key"));
    address.stringValue=@KEY;
    CHECK(!Send(review) && !window.attachedSheet);
    worker.state=MDHostsSynced;
    CHECK([NSApp sendAction:@selector(refresh:) to:page from:refresh]);
    CHECK(Await(^BOOL{ return review.enabled; }));
    worker.mismatch=YES;
    CHECK(Send(review) && Await(^BOOL{ return window.attachedSheet!=nil; }));
    sheet=window.attachedSheet.contentView;
    CHECK(Button(sheet,@"Keep Old Key") && Button(sheet,@"Replace Key") && !Button(sheet,@"Connect") && !Button(sheet,@"Save Host"));
    NSTextField *fp=Field(sheet,@"Type the new fingerprint"); CHECK(fp);
    /* S4: the confirmation is typed only. Paste, drop and Services insertion
     * are refused, and the new fingerprint is not offered for copying. */
    NSWindow *sheetWindow=window.attachedSheet;
    CHECK(Refuses(sheetWindow, fp, nil, nil));
    NSTextField *shown=(NSTextField *)Find(sheet,^BOOL(NSView *v){ return [v isKindOfClass:NSTextField.class] &&
        !((NSTextField *)v).editable && [((NSTextField *)v).stringValue isEqual:MDHostFingerprint(@KEY)]; });
    CHECK(shown && !shown.selectable);
    CHECK([sheetWindow makeFirstResponder:fp]);
    MDKeyEditor *typed=(MDKeyEditor *)fp.currentEditor;
    CHECK(typed.typedOnly && !typed.acceptableDragTypes.count && ![typed validRequestorForSendType:nil returnType:NSPasteboardTypeString]);
    NSPasteboard *pb=[NSPasteboard pasteboardWithUniqueName]; MDSetEditingPasteboard(pb);
    [pb clearContents]; [pb setString:MDHostFingerprint(@KEY) forType:NSPasteboardTypeString];
    CHECK(!Enabled(sheetWindow,@selector(paste:)));
    [typed paste:nil];
    CHECK(![typed readSelectionFromPasteboard:pb] && ![typed readSelectionFromPasteboard:pb type:NSPasteboardTypeString]);
    CHECK(!fp.stringValue.length && !Button(sheet,@"Replace Key").enabled);
    [typed insertText:MDHostFingerprint(@KEY) replacementRange:typed.selectedRange];
    CHECK([fp.stringValue isEqual:MDHostFingerprint(@KEY)] && Button(sheet,@"Replace Key").enabled);
    typed.string=@""; [sheetWindow.undoManager removeAllActions]; [pb releaseGlobally]; MDSetEditingPasteboard(nil);
    [sheetWindow makeFirstResponder:nil];
    Typed(fp,@"");
    KeyWindowOverride=window;
    /* Replace stays off until the exact new fingerprint is typed. */
    CHECK(NoDefault(sheet) && Escapes(Button(sheet,@"Keep Old Key")) && !Button(sheet,@"Replace Key").enabled);
    CHECK(Button(sheet,@"Replace Key").hasDestructiveAction);
    Typed(fp,@"wrong"); CHECK(!Button(sheet,@"Replace Key").enabled);
    Typed(fp,MDHostFingerprint(@OTHER)); CHECK(!Button(sheet,@"Replace Key").enabled);
    Typed(fp,MDHostFingerprint(@KEY)); CHECK(Button(sheet,@"Replace Key").enabled);
    CHECK(EndSheet(window,NSModalResponseCancel) && worker.saves==1);
    CHECK(Send(review) && Await(^BOOL{ return window.attachedSheet!=nil; }));
    fp=Field(window.attachedSheet.contentView,@"Type the new fingerprint"); CHECK(fp); fp.stringValue=@"wrong";
    CHECK(EndSheet(window,NSAlertSecondButtonReturn));
    CHECK(LabelContains(view,@"Replacing it needs the exact new fingerprint") && worker.saves==1 && worker.launches==1);
    CHECK(Send(review) && Await(^BOOL{ return window.attachedSheet!=nil; }));
    fp=Field(window.attachedSheet.contentView,@"Type the new fingerprint"); CHECK(fp); fp.stringValue=MDHostFingerprint(@KEY);
    CHECK(EndSheet(window,NSAlertSecondButtonReturn));
    CHECK(Await(^BOOL{ return LabelContains(view,@"Key replaced"); }));
    CHECK(worker.saves==2 && [worker.savedReview.oldKey isEqual:@OTHER] && [worker.confirmation isEqual:MDHostFingerprint(@KEY)]);
    return YES;
}

@interface FixtureLink : MDHostLink
@property(copy) MDHostUpdate push;
@property(strong) NSMutableArray<NSDictionary *> *commands;
@property(strong) NSMutableArray<NSString *> *paths;
@property BOOL closed;
@end
@implementation FixtureLink
- (void)command:(NSDictionary *)cmd policyVersion:(NSNumber *)version {
    MD_CHECK(cmd!=nil && !version); if (cmd) [self.commands addObject:cmd];
}
- (void)close:(dispatch_block_t)drained { self.closed=YES; drained(); }
- (void)connect:(NSString *)path { [self.paths addObject:path ?: @""]; }
@end
static NSDictionary *Snapshot(NSInteger revision,uint32_t requested, NSTimeInterval expiry, BOOL removed) {
    NSDictionary *r=@{@"request_id":@REQUEST,@"peer":@KEY,@"requested":@(requested),@"granted":@0,
        @"state":@"pending",@"expires_at":@(expiry)};
    /* A second client is always waiting too: every action must stay bound
     * to the request the user chose. */
    NSDictionary *other=@{@"request_id":@REQUEST2,@"peer":@OTHER,@"requested":@(MD_CAP_VIDEO),@"granted":@0,
        @"state":@"pending",@"expires_at":@(NSDate.date.timeIntervalSince1970+600)};
    return @{@"v":@1,@"ok":@YES,@"generation":@GEN,@"revision":@(revision),@"session":NSNull.null,
        @"requests":removed?@[other]:@[r,other],@"supported":@(MD_CAP_VIDEO|MD_CAP_AGENT)};
}
/* Stands in for UNNotificationResponse: the delegate reads only these. */
@interface FixtureRequest : NSObject
@property(copy) NSString *identifier;
@end
@implementation FixtureRequest
@end
@interface FixtureNotification : NSObject
@property(strong) FixtureRequest *request;
@end
@implementation FixtureNotification
@end
@interface FixtureResponse : NSObject
@property(strong) FixtureNotification *notification;
@property(copy) NSString *actionIdentifier;
+ (instancetype)responseWithToken:(NSString *)token opened:(BOOL)opened;
@end
@implementation FixtureResponse
+ (instancetype)responseWithToken:(NSString *)token opened:(BOOL)opened {
    FixtureResponse *r=[FixtureResponse new]; r.notification=[FixtureNotification new];
    r.notification.request=[FixtureRequest new]; r.notification.request.identifier=token;
    r.actionIdentifier=opened ? UNNotificationDefaultActionIdentifier : UNNotificationDismissActionIdentifier;
    return r;
}
@end
static BOOL Access(MDLocalHostPages *page, FixtureLink *link, NSWindow *window) {
    NSButton *open=Button(page.accessView,@"Review Request…");
    CHECK(open && !open.enabled);
    const uint32_t all=MD_CAP_VIDEO|MD_CAP_AGENT|MD_CAP_INPUT;
    NSTimeInterval now=NSDate.date.timeIntervalSince1970;
    NSDictionary *policy=@{@"policy_available":@YES,@"policy_version":@4,@"clients":@[]};
    link.push(@"Fixture",Snapshot(1,all,now+60,NO),policy,nil,NO);
    CHECK(Send(open) && Await(^BOOL{ return window.attachedSheet!=nil; }));
    NSView *sheet=window.attachedSheet.contentView;
    NSButton *video=Button(sheet,MDAccessCaps(MD_CAP_VIDEO)), *agent=Button(sheet,MDAccessCaps(MD_CAP_AGENT));
    NSButton *input=Button(sheet,MDAccessCaps(MD_CAP_INPUT));
    CHECK(video && video.enabled && video.state==NSControlStateValueOn);
    CHECK(agent && agent.enabled && agent.state==NSControlStateValueOff);
    CHECK(input && !input.enabled && input.state==NSControlStateValueOff);
    CHECK(Button(sheet,@"Approve Once").enabled && Button(sheet,@"Approve and Remember").enabled);
    CHECK(NoDefault(sheet) && Escapes(Button(sheet,@"Cancel")));
    CHECK(EndSheet(window,NSAlertSecondButtonReturn));
    CHECK(link.commands.count==1);
    NSDictionary *command=link.commands.firstObject;
    CHECK([command[@"decision"] isEqual:@"approve"] && [command[@"caps"] unsignedIntValue]==MD_CAP_VIDEO && ![command[@"remember"] boolValue]);
    CHECK([command[@"generation"] isEqual:@GEN] && [command[@"request_id"] isEqual:@REQUEST]);
    for (NSInteger round=0;round<2;++round) {
        link.push(@"Fixture",Snapshot(2+round,MD_CAP_AGENT,now+60,NO),policy,nil,NO);
        CHECK(Send(open) && Await(^BOOL{ return window.attachedSheet!=nil; }));
        sheet=window.attachedSheet.contentView;
        CHECK(Button(sheet,MDAccessCaps(MD_CAP_VIDEO)).state==NSControlStateValueOff && Button(sheet,@"Deny").enabled);
        link.push(@"Fixture",Snapshot(2+round,MD_CAP_AGENT,round ? now+60 : now-1,round!=0),policy,nil,NO);
        CHECK(!open.enabled && !Button(sheet,@"Approve Once").enabled && !Button(sheet,@"Approve and Remember").enabled && !Button(sheet,@"Deny").enabled);
        CHECK(EndSheet(window,NSAlertThirdButtonReturn+1) && link.commands.count==1);
    }
    /* B1: with A selected, B's notification opens B's review and nothing else;
     * a notification for a request that has gone opens nothing. */
    link.push(@"Fixture",Snapshot(10,all,now+60,NO),policy,nil,NO);
    NSTableView *requests=(NSTableView *)Find(page.accessView,^BOOL(NSView *v){ return [v isKindOfClass:NSTableView.class] && [v.accessibilityLabel isEqual:@"Pending requests"]; });
    CHECK(requests.numberOfRows==2);
    [requests selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [page openRequest:@GEN "/" REQUEST2];
    CHECK(Await(^BOOL{ return window.attachedSheet!=nil; }));
    sheet=window.attachedSheet.contentView;
    CHECK(LabelContains(sheet,MDHostFingerprint(@OTHER)) && !LabelContains(sheet,MDHostFingerprint(@KEY)) && requests.selectedRow==1);
    CHECK(EndSheet(window,NSModalResponseCancel) && link.commands.count==1);
    /* The notification callback itself: B's response opens B's review. */
    [requests selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    __block BOOL completed=NO;
    id response=[FixtureResponse responseWithToken:@GEN "/" REQUEST2 opened:YES];
    [(id<UNUserNotificationCenterDelegate>)page userNotificationCenter:UNUserNotificationCenter.currentNotificationCenter
        didReceiveNotificationResponse:response withCompletionHandler:^{ completed=YES; }];
    CHECK(Await(^BOOL{ return completed && window.attachedSheet!=nil; }));
    CHECK(LabelContains(window.attachedSheet.contentView,MDHostFingerprint(@OTHER)) && requests.selectedRow==1);
    CHECK(EndSheet(window,NSModalResponseCancel) && link.commands.count==1);
    /* A dismissed notification opens nothing. */
    completed=NO;
    [(id<UNUserNotificationCenterDelegate>)page userNotificationCenter:UNUserNotificationCenter.currentNotificationCenter
        didReceiveNotificationResponse:(id)[FixtureResponse responseWithToken:@GEN "/" REQUEST2 opened:NO] withCompletionHandler:^{ completed=YES; }];
    CHECK(Await(^BOOL{ return completed; }) && !window.attachedSheet);
    [requests selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [page openRequest:@GEN "/" "0123456789abcdef0123456789abcdef"];
    Turn();
    CHECK(!window.attachedSheet && requests.selectedRow==0);
    for (NSInteger busy=0; busy<2; ++busy) {
        link.push(@"Fixture",Snapshot(8,all,now+60,NO),policy,nil,NO);
        CHECK(Send(open) && Await(^BOOL{ return window.attachedSheet!=nil; }));
        sheet=window.attachedSheet.contentView;
        CHECK(Button(sheet,@"Approve Once").enabled);
        link.push(@"Fixture",Snapshot(busy ? 8 : 9,all,now+60,NO),policy,nil,busy!=0);
        CHECK(!Button(sheet,@"Approve Once").enabled && !Button(sheet,@"Approve and Remember").enabled && !Button(sheet,@"Deny").enabled);
        CHECK(EndSheet(window,NSAlertSecondButtonReturn) && link.commands.count==1);
    }
    return YES;
}

static atomic_uint connectCalls, pairWrites;
static int GetPair(void *unused,const char *key,char **payload) {
    (void)unused; (void)key; *payload=NULL; return MD_CREDENTIAL_NOT_FOUND;
}
static int PutPair(void *unused,const char *key,const char *payload) {
    (void)unused;
    MdNip46Credentials *pair = NULL;
    int rc = md_nip46_credentials_parse(payload,&pair);
    if (!rc && !strcmp(key,OTHER) && !strcmp(pair->user_pubkey,OTHER) && !strcmp(pair->remote_pubkey,KEY))
        atomic_fetch_add(&pairWrites,1);
    else rc = MD_CREDENTIAL_INVALID;
    md_nip46_credentials_free(pair);
    return rc;
}
static int RemovePair(void *unused,const char *key) { (void)unused; (void)key; return 0; }
@interface FixtureIdentity : MDIdentityPage
@property(strong) NSURL *pairingFile;
@end
@implementation FixtureIdentity
- (void)choosePairingFile:(NSWindow *)window completion:(void (^)(NSURL *))completion {
    (void)window; completion(self.pairingFile);
}
@end
static int FixtureKey(MdSigner *s,char **key) { (void)s; *key=strdup(KEY); return *key?0:MD_SIGNER_ERR_INTERNAL; }
static int Connect(MdSignerType backend,const char *key,MdCredentialStore *store,MdSigner **out) {
    if (!store || strcmp(key,OTHER)) return MD_SIGNER_ERR_CONNECT;
    static const MdSignerOps ops={.get_pubkey=FixtureKey};
    atomic_fetch_add(&connectCalls,1);
    *out=md_signer_create_custom(backend,&ops,NULL); return *out?0:MD_SIGNER_ERR_INTERNAL;
}
static BOOL Identity(FixtureIdentity *page, NSString *directory, MdSignerType backend) {
    NSView *view=page.view;
    NSWindow *window=view.window;
    NSButton *bind=Button(view,@"Use for Client"), *add=Button(view,@"Add Account…");
    CHECK(bind && !bind.enabled);
    [page activate]; CHECK(Await(^BOOL{ return add.enabled; }));
    CHECK(Send(add) && Await(^BOOL{ return window.attachedSheet!=nil; }));
    NSView *form=window.attachedSheet.contentView;
    NSPopUpButton *picker=(NSPopUpButton *)Find(form,^BOOL(NSView *v){ return [v isKindOfClass:NSPopUpButton.class]; });
    NSTextField *key=Field(form,@"Public key"), *label=Field(form,@"Public label");
    CHECK(picker && key && label && [picker selectItemWithTag:backend]);
    NSWindow *sheetWindow=window.attachedSheet;
    CHECK(Refuses(sheetWindow, key, form, @"That’s a private key"));
    Typed(key,@"");
    NSButton *addButton=Button(form,@"Add Account");
    CHECK(addButton && !addButton.enabled);
    CHECK([sheetWindow makeFirstResponder:key]);
    [(NSTextView *)key.currentEditor insertText:@OTHER replacementRange:NSMakeRange(0,0)];
    CHECK(addButton.enabled);
    label.stringValue=@"Candidate";
    [addButton performClick:nil];
    CHECK(Await(^BOOL{ return window.attachedSheet==nil; }));
    KeyWindowOverride=window;
    CHECK(Await(^BOOL{ return bind.enabled && LabelContains(view,@"Account added"); }));
    CHECK([MDIdentityAccounts(directory,NULL).firstObject[@"key"] isEqual:@OTHER]);
    CHECK(Send(Button(view,@"Edit Label…")) && Await(^BOOL{ return window.attachedSheet!=nil; }));
    label=Field(window.attachedSheet.contentView,@"Public label"); CHECK(label);
    label.stringValue=@"Renamed";
    CHECK(EndSheet(window,NSAlertFirstButtonReturn));
    CHECK(Await(^BOOL{ return LabelContains(view,@"Label saved"); }));
    CHECK([MDIdentityAccounts(directory,NULL).firstObject[@"label"] isEqual:@"Renamed"]);
    if (backend==MD_SIGNER_NIP46) {
        NSURL *file=[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:@"fixture-pairing.json"]];
        NSString *json=[NSString stringWithFormat:@"{\"version\":1,\"user_pubkey\":\"%s\",\"remote_pubkey\":\"%s\",\"client_secret\":\"%064d\",\"relays\":[\"wss://fixture.invalid/\"]}",OTHER,KEY,1];
        CHECK([json writeToURL:file atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
        NSURL *link=[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:@"fixture-link.json"]];
        CHECK(!symlink(file.fileSystemRepresentation,link.fileSystemRepresentation));
        page.pairingFile=link;
        NSButton *pair=Button(view,@"Import Pairing…");
        CHECK(Send(pair) && Await(^BOOL{ return window.attachedSheet!=nil; }));
        CHECK(EndSheet(window,NSAlertFirstButtonReturn));
        CHECK(Await(^BOOL{ return LabelContains(view,@"not a symbolic link"); }) && !atomic_load(&pairWrites));
        page.pairingFile=file;
        CHECK(Send(pair) && Await(^BOOL{ return window.attachedSheet!=nil; }));
        CHECK(EndSheet(window,NSAlertFirstButtonReturn));
        CHECK(Await(^BOOL{ return LabelContains(view,@"Pairing saved in Keychain"); }));
        CHECK(atomic_load(&pairWrites)==1 && ![MDIdentityAccounts(directory,NULL).firstObject[@"client"] boolValue]);
        CHECK(!unlink(link.fileSystemRepresentation) && !unlink(file.fileSystemRepresentation));
    }
    /* S5: another writer puts an account ahead of the selected one; after a
     * reload the selection and Use for Client still mean the same account. */
    static const MdCredentialStoreOps ops={.get=GetPair,.put=PutPair,.remove=RemovePair};
    MdCredentialStore *store=md_credential_store_custom(&ops,NULL);
    NSDictionary *selected=MDIdentityAccounts(directory,NULL).firstObject;
    CHECK(!MDIdentityRemove(directory,store,selected).length);
    CHECK(!MDIdentityAdd(directory,backend,@THIRD,@"Inserted ahead").length);
    CHECK(!MDIdentityAdd(directory,backend,@OTHER,@"Renamed").length);
    NSTableView *accounts=(NSTableView *)Find(view,^BOOL(NSView *v){ return [v isKindOfClass:NSTableView.class] && [v.accessibilityLabel isEqual:@"Signer accounts"]; });
    CHECK([[MDIdentityAccounts(directory,NULL)[0] objectForKey:@"key"] isEqual:@THIRD]);
    [page refresh:nil];
    CHECK(Await(^BOOL{ return bind.enabled && accounts.numberOfRows==2; }));
    CHECK(accounts.selectedRow==1 && LabelContains(view,MDHostNpub(@OTHER)));
    CHECK(Send(bind));
    CHECK(!bind.enabled);
    CHECK(Await(^BOOL{ return bind.enabled && LabelContains(page.view,@"different public key"); }));
    CHECK(atomic_load(&connectCalls)==1);
    CHECK(![MDIdentityAccounts(directory,NULL)[1][@"client"] boolValue]);
    /* The selected account disappears: nothing else becomes the target. */
    CHECK(!MDIdentityRemove(directory,store,MDIdentityAccounts(directory,NULL)[1]).length);
    md_credential_store_destroy(store);
    [page refresh:nil];
    CHECK(Await(^BOOL{ return accounts.numberOfRows==1 && !LabelContains(view,@"Loading"); }));
    CHECK(Await(^BOOL{ return [Button(view,@"Add Account…") isEnabled]; }));
    CHECK(accounts.selectedRow==-1 && !bind.enabled);
    return YES;
}
/* An account whose signer type this build doesn't include (saved by another
 * build) is shown, but nothing the build can't do is offered: no Use for
 * Client/Host, no Import Pairing, and a note saying why. */
static BOOL UnbuiltSigner(MDIdentityPage *page, NSString *directory) {
    MdSignerType types[]={MD_SIGNER_NIP46, MD_SIGNER_NIP5F, MD_SIGNER_NIP55L}, missing=MD_SIGNER_DIRECT_KEY;
    for (size_t i=0;i<3;++i) if (!MDIdentitySupportsBackend(types[i])) { missing=types[i]; break; }
    if (missing==MD_SIGNER_DIRECT_KEY) { puts("SKIP: every external signer type is built"); return YES; }
    const char *key="c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";
    MdCredentialStore *store=md_credential_store_memory(); MdAccounts *accounts=NULL;
    CHECK(store && !md_accounts_open(directory.fileSystemRepresentation,store,&accounts));
    CHECK(!md_accounts_add(accounts,missing,key,"From another build") && !md_accounts_save(accounts));
    md_accounts_destroy(accounts); md_credential_store_destroy(store);
    /* Read the expected list once: polling the accounts store while the
     * page reloads would contend for its lock. */
    NSArray *rows=MDIdentityAccounts(directory,NULL);
    NSUInteger row=[rows indexOfObjectPassingTest:^BOOL(NSDictionary *a, NSUInteger i, BOOL *stop) { (void)i; (void)stop; return [a[@"key"] isEqual:@(key)]; }];
    CHECK(row!=NSNotFound);
    CHECK(Await(^BOOL{ return [page validateMenuItem:MenuItem(NSApp.mainMenu,@selector(refresh:))]; }));
    [page refresh:nil];
    NSTableView *table=(NSTableView *)Find(page.view,^BOOL(NSView *v){ return [v isKindOfClass:NSTableView.class] && [v.accessibilityLabel isEqual:@"Signer accounts"]; });
    CHECK(Await(^BOOL{ return table.numberOfRows==(NSInteger)rows.count && [page validateMenuItem:MenuItem(NSApp.mainMenu,@selector(refresh:))]; }));
    [table selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    NSView *view=page.view;
    CHECK(![Button(view,@"Use for Client") isEnabled] && ![Button(view,@"Use for Host") isEnabled]);
    CHECK(!Visible(Button(view,@"Import Pairing…")));
    CHECK(LabelContains(view,@"built without support for"));
    /* A supported account selected again: the note goes away. */
    [table selectRowIndexes:[NSIndexSet indexSetWithIndex:row ? 0 : 1] byExtendingSelection:NO];
    if (table.numberOfRows>1) CHECK(!LabelContains(view,@"built without support for"));
    return YES;
}
@interface FixtureApp : MDManagerApp
@property NSUInteger replies;
@end
@implementation FixtureApp
- (void)replyToTermination { ++self.replies; }
@end
static BOOL Quit(FixtureApp *app, MDHostsPage *page, FixtureHosts *worker, FixtureLink *link, NSWindow *window) {
    NSButton *review=Button(page.view,@"Review and Connect…");
    CHECK(Await(^BOOL{ return review.enabled; }));
    Field(page.view,@"Host npub").stringValue=@KEY;
    __block BOOL entered=NO;
    worker.entered=^{ entered=YES; };
    worker.proceed=dispatch_semaphore_create(0);
    NSUInteger old=worker.closes;
    CHECK(Send(review) && Await(^BOOL{ return entered; }));
    CHECK([app applicationShouldTerminate:NSApp]==NSTerminateLater);
    CHECK([app applicationShouldTerminate:NSApp]==NSTerminateLater && !app.replies && link.closed && worker.closes==old);
    dispatch_semaphore_signal(worker.proceed);
    CHECK(Await(^BOOL{ return app.replies!=0; }));
    CHECK(app.replies==1 && worker.closes==old+1 && worker.observeCancellation && !window.attachedSheet);
    CHECK([app applicationShouldTerminate:NSApp]==NSTerminateNow);
    return YES;
}
/* S6: during a client-role edit or with a sheet open, the Hosts commands are
 * disabled in the menu and their handlers do nothing, not even switch section. */
static BOOL Commands(MDManagerApp *app, NSWindow *window, MDHostsPage *hosts) {
    [app showSection:@"Hosts"];
    CHECK(Enabled(window,@selector(connectToHost:)));
    __block BOOL drained=NO;
    [hosts suspendForRoleEdit:^{ drained=YES; }];
    CHECK(Await(^BOOL{ return drained; }));
    [app showSection:@"Identity"];
    CHECK(!Enabled(window,@selector(connectToHost:)) && !Enabled(window,@selector(importHosts:)) && !Enabled(window,@selector(scanQR:)));
    [app performSelector:@selector(connectToHost:) withObject:nil];
    CHECK([window.title isEqual:@"Identity"]);
    [hosts resumeAfterRoleEdit];
    [app showSection:@"Hosts"];
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(importHosts:)]; }));
    NSAlert *alert=[NSAlert new]; alert.messageText=@"Fixture"; [alert addButtonWithTitle:@"OK"];
    [alert beginSheetModalForWindow:window completionHandler:nil];
    CHECK(Await(^BOOL{ return window.attachedSheet!=nil; }));
    NSWindow *sheet=window.attachedSheet;
    CHECK(!Enabled(window,@selector(importHosts:)) && !Enabled(window,@selector(connectToHost:)) && !Enabled(window,@selector(showSection:)));
    [hosts importHosts:nil];
    [app performSelector:@selector(connectToHost:) withObject:nil];
    CHECK(window.attachedSheet==sheet);
    CHECK(EndSheet(window,NSAlertFirstButtonReturn));
    return YES;
}
/* S8: only the window frame is remembered. */
static BOOL Persistence(MDManagerApp *app, NSWindow *window) {
    [app showSection:@"Hosts"];
    CHECK(!window.isRestorable && window.frameAutosaveName.length);
    NSSplitView *split=(NSSplitView *)Find(window.contentView.superview ?: window.contentView,^BOOL(NSView *v){ return [v isKindOfClass:NSSplitView.class]; });
    CHECK(split && !split.autosaveName.length);
    MDMainWindowController *controller=window.windowController;
    NSSearchField *search=controller.searchField;
    CHECK(search && !search.recentsAutosaveName.length && search.maximumRecents==0);
    return YES;
}
/* P4: before Hosts is shown it shows the opening panel and nothing is enabled. */
static BOOL BeforeActivation(MDHostsPage *hosts) {
    NSView *view=hosts.view;
    NSButton *review=Button(view,@"Review and Connect…");
    CHECK(review && !review.enabled && !Button(view,@"Scan QR Code…").enabled);
    CHECK(!view.window || !Visible(Field(view,@"Host npub")));
    MDSetupPanel *panel=(MDSetupPanel *)SetupPanel(view);
    CHECK(panel && !panel.isHidden && [panel.titleLabel.stringValue containsString:@"Opening"]);
    return YES;
}
/* F1: Save and Sync on the Relays page presents its own confirmation and
 * reports into the Relays page, whichever section is not visible. */
static BOOL RelaysSave(MDManagerApp *app, NSWindow *window) {
    NSString *prefs=[[@(getenv("XDG_CONFIG_HOME")) stringByAppendingPathComponent:@"metadesk"] stringByAppendingPathComponent:@"manager-relays.ini"];
    NSString *corrupt=@"not a relay preferences file";
    CHECK([corrupt writeToFile:prefs atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    [app showSection:@"This Computer"];
    [app showSection:@"Relays"];
    NSView *root=window.contentView;
    NSTextField *relays=Field(root,@"Bootstrap relays");
    NSButton *save=Button(root,@"Save and Sync");
    CHECK(relays && save && Visible(save));
    NSString *(^saved)(void)=^NSString *{ return [NSString stringWithContentsOfFile:prefs encoding:NSUTF8StringEncoding error:NULL]; };
    /* The page reloads on appearing and says the saved file is unreadable. */
    CHECK(Await(^BOOL{ return save.enabled && LabelContains(root,@"can’t be read"); }));
    relays.stringValue=@"wss://relay-one.fixture.invalid/";
    /* Cancel keeps the unreadable file. */
    [save performClick:nil];
    CHECK(Await(^BOOL{ return window.attachedSheet!=nil; }) && Button(window.attachedSheet.contentView,@"Keep Existing File"));
    CHECK(EndSheet(window,NSAlertFirstButtonReturn));
    CHECK(Await(^BOOL{ return LabelContains(root,@"kept unchanged"); }) && [saved() isEqual:corrupt]);
    /* Confirmed replacement writes the relays in the field. */
    CHECK(Await(^BOOL{ return save.enabled; }));
    [save performClick:nil];
    CHECK(Await(^BOOL{ return window.attachedSheet!=nil; }));
    CHECK(EndSheet(window,NSAlertSecondButtonReturn));
    CHECK(Await(^BOOL{ return [saved() containsString:@"relay-one.fixture.invalid"]; }));
    CHECK(Await(^BOOL{ return LabelContains(root,@"Relays saved"); }));
    /* Invalid input is explained in the Relays page and saves nothing. */
    relays.stringValue=@"https://not-a-relay.fixture.invalid/";
    CHECK(Await(^BOOL{ return save.enabled; }));
    [save performClick:nil];
    CHECK(Await(^BOOL{ return LabelContains(root,@"Use ws:// or wss:// URLs"); }) && [saved() containsString:@"relay-one.fixture.invalid"]);
    CHECK(!window.attachedSheet);
    /* Clearing removes every relay. */
    relays.stringValue=@"";
    CHECK(Await(^BOOL{ return save.enabled; }));
    [save performClick:nil];
    CHECK(Await(^BOOL{ return LabelContains(root,@"Relays cleared"); }) && ![saved() containsString:@"relay-one"]);
    return YES;
}
/* F2: first-run guidance follows accounts, and its buttons reach the app. */
static BOOL Onboarding(MDManagerApp *app, NSWindow *window, MDHostsPage *hosts, FixtureHosts *worker, MDIdentityPage *identity, NSString *directory) {
    hosts.accountCount=^NSUInteger{ return MDIdentityAccounts(directory,NULL).count; };
    worker.state=MDHostsNoClientRole;
    [app showSection:@"Hosts"];
    [NSApp sendAction:@selector(refresh:) to:hosts from:nil];
    MDSetupPanel *panel=(MDSetupPanel *)SetupPanel(hosts.view);
    CHECK(Await(^BOOL{ return Visible(panel) && [panel.titleLabel.stringValue isEqual:@"Add Your Signer Account"]; }));
    CHECK(!Visible(Field(hosts.view,@"Host npub")));
    /* P2: Connect to Host can't focus a hidden field, so it is disabled. */
    CHECK(!Enabled(window,@selector(connectToHost:)) && !Enabled(window,@selector(importHosts:)));
    /* The panel's button opens Identity's Add Account sheet. */
    [panel.primary performClick:nil];
    CHECK(Await(^BOOL{ return window.attachedSheet!=nil; }) && [window.title isEqual:@"Identity"]);
    NSWindow *sheet=window.attachedSheet;
    NSTextField *key=Field(sheet.contentView,@"Public key");
    CHECK(key && [sheet makeFirstResponder:key]);
    [(NSTextView *)key.currentEditor insertText:@THIRD replacementRange:NSMakeRange(0,0)];
    NSButton *add=Button(sheet.contentView,@"Add Account");
    CHECK(add.enabled);
    [add performClick:nil];
    CHECK(Await(^BOOL{ return !window.attachedSheet && MDIdentityAccounts(directory,NULL).count==1; }));
    KeyWindowOverride=window;
    /* Not assigned yet: Hosts now asks to choose an account, without a visit. */
    CHECK(Await(^BOOL{ return [panel.titleLabel.stringValue isEqual:@"Choose an Account for Connecting"]; }));
    [app showSection:@"Hosts"];
    CHECK(Visible(panel));
    [panel.primary performClick:nil];
    CHECK([window.title isEqual:@"Identity"]);
    /* Back to the ready state for the remaining tests. */
    static const MdCredentialStoreOps ops={.get=GetPair,.put=PutPair,.remove=RemovePair};
    MdCredentialStore *store=md_credential_store_custom(&ops,NULL);
    CHECK(!MDIdentityRemove(directory,store,MDIdentityAccounts(directory,NULL).firstObject).length);
    md_credential_store_destroy(store);
    [identity refresh:nil];
    CHECK(Await(^BOOL{ return [identity validateMenuItem:MenuItem(NSApp.mainMenu,@selector(refresh:))]; }));
    worker.state=MDHostsSynced;
    hosts.accountCount=nil;
    [app showSection:@"Hosts"];
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)]; }));
    [NSApp sendAction:@selector(refresh:) to:hosts from:nil];
    CHECK(Await(^BOOL{ return Visible(Field(hosts.view,@"Host npub")) && !Visible(panel); }));
    return YES;
}
/* P1: Search Hosts uses the refusing field editor. P2: with a sheet open in
 * another section, Hosts commands are disabled. */
static BOOL SearchAndSheets(MDManagerApp *app, NSWindow *window) {
    [app showSection:@"Hosts"];
    MDMainWindowController *controller=window.windowController;
    NSSearchField *search=controller.searchField;
    CHECK(Refuses(window, search, nil, nil));
    [app showSection:@"Identity"];
    NSAlert *alert=[NSAlert new]; alert.messageText=@"Fixture"; [alert addButtonWithTitle:@"OK"];
    [alert beginSheetModalForWindow:window completionHandler:nil];
    CHECK(Await(^BOOL{ return window.attachedSheet!=nil; }));
    for (NSString *name in @[@"importHosts:", @"exportHosts:", @"scanQR:", @"connectToHost:", @"findHosts:"])
        CHECK(!Enabled(window, NSSelectorFromString(name)));
    CHECK(EndSheet(window,NSAlertFirstButtonReturn));
    return YES;
}
/* A discovery lookup with one fixed answer. If gate is set, the lookup
 * signals entered and then blocks until gate is released (the barrier),
 * so the test controls exactly when its answer lands. */
typedef NSString *(^Discovery)(NSString **);
static Discovery Answer(NSString *key, NSString *path, dispatch_semaphore_t entered, dispatch_semaphore_t gate, atomic_int *calls) {
    return ^NSString *(NSString **hostKey) {
        if (calls) atomic_fetch_add(calls,1);
        if (entered) dispatch_semaphore_signal(entered);
        if (gate) MD_CHECK(dispatch_semaphore_wait(gate, dispatch_time(DISPATCH_TIME_NOW, 8*NSEC_PER_SEC))==0);
        *hostKey=key;
        return path;
    };
}
static BOOL Entered(dispatch_semaphore_t entered) {
    return dispatch_semaphore_wait(entered, dispatch_time(DISPATCH_TIME_NOW, 4*NSEC_PER_SEC))==0;
}
static NSTableView *Requests(MDLocalHostPages *page) {
    return (NSTableView *)Find(page.accessView,^BOOL(NSView *v){ return [v isKindOfClass:NSTableView.class] && [v.accessibilityLabel isEqual:@"Pending requests"]; });
}
/* F3 and round 4 #2: discovery follows the host role. A role change fences
 * the old link, its requests and any open decision before the replacement is
 * looked up; late answers for an earlier role are dropped; a custom socket
 * works with no host role, and leaving it always looks up afresh. */
static BOOL ComputerRole(MDManagerApp *app, MDLocalHostPages *local, FixtureLink *link, NSWindow *window, MDIdentityPage *identity) {
    [app showSection:@"This Computer"];
    NSString *keyA=@"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", *keyB=@"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    NSString *keyC=@"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    NSTimeInterval now=NSDate.date.timeIntervalSince1970;
    NSDictionary *policy=@{@"policy_available":@YES,@"policy_version":@4,@"clients":@[]};
    const uint32_t all=MD_CAP_VIDEO|MD_CAP_AGENT|MD_CAP_INPUT;
    /* Role A: found and connected. */
    local.discover=Answer(keyA,@"/fixture/role-a.sock",nil,nil,NULL);
    [local hostRoleChanged:keyA];
    CHECK(Await(^BOOL{ return [link.paths.lastObject isEqual:@"/fixture/role-a.sock"]; }));
    /* Role A is unchanged (a label edit): no lookup, the link stays. */
    atomic_int idle=0;
    local.discover=Answer(keyA,@"/fixture/other.sock",nil,nil,&idle);
    NSUInteger connects=link.paths.count;
    [local hostRoleChanged:keyA];
    Turn();
    CHECK(atomic_load(&idle)==0 && link.paths.count==connects);
    /* An active snapshot from role A with an approval sheet open. */
    link.push(@"Fixture",Snapshot(20,all,now+60,NO),policy,nil,NO);
    NSTableView *requests=Requests(local);
    CHECK(requests.numberOfRows==2);
    [app showSection:@"Access"];
    [local openRequest:@GEN "/" REQUEST];
    CHECK(Await(^BOOL{ return window.attachedSheet!=nil; }));
    NSUInteger commands=link.commands.count;
    /* Role B: its lookup blocks. Before it answers, role A's link, requests,
     * remembered clients and decision sheet are already gone. */
    dispatch_semaphore_t enteredB=dispatch_semaphore_create(0), gateB=dispatch_semaphore_create(0);
    local.discover=Answer(keyB,@"/fixture/role-b.sock",enteredB,gateB,NULL);
    [local hostRoleChanged:keyB];
    CHECK(Entered(enteredB));
    CHECK(!window.attachedSheet && link.commands.count==commands);
    CHECK([link.paths.lastObject isEqual:@""] && requests.numberOfRows==0);
    CHECK(!Button(local.accessView,@"Review Request…").enabled && LabelContains(local.accessView,@"Not Connected"));
    /* The dropped link's own failure doesn't replace "looking for the host". */
    link.push(@"Host unreachable or socket not trusted. Agent process state is unknown.",nil,nil,nil,NO);
    CHECK(LabelContains(local.computerView,@"Looking for the host on this Mac") && !LabelContains(local.computerView,@"socket not trusted"));
    /* A late update for role A's request can't be approved from a stale sheet. */
    [local openRequest:@GEN "/" REQUEST];
    Turn();
    CHECK(!window.attachedSheet);
    /* Role C arrives while B is still blocked; B's late answer is dropped.
     * The discovery queue is serial, so B's answer reaches the main queue
     * before C's: once C's path is in use, B's has been seen and ignored. */
    local.discover=Answer(keyC,@"/fixture/role-c.sock",nil,nil,NULL);
    [local hostRoleChanged:keyC];
    dispatch_semaphore_signal(gateB);
    CHECK(Await(^BOOL{ return [link.paths.lastObject isEqual:@"/fixture/role-c.sock"]; }));
    CHECK(![link.paths containsObject:@"/fixture/role-b.sock"]);
    /* Unassigning the role, as Identity reports it after a reload: the link
     * is dropped and the page says so. The fixture directory has no host role. */
    local.discover=Answer(nil,nil,nil,nil,NULL);
    CHECK(Await(^BOOL{ return [identity validateMenuItem:MenuItem(NSApp.mainMenu,@selector(refresh:))]; }));
    [identity refresh:nil];
    CHECK(Await(^BOOL{ return [link.paths.lastObject isEqual:@""] && LabelContains(local.computerView,@"doesn’t have a host role"); }));
    /* Role A again, then a custom socket: an explicit choice that works
     * without a host role and with an active snapshot of its own. */
    [app showSection:@"This Computer"];
    local.discover=Answer(keyA,@"/fixture/role-a.sock",nil,nil,NULL);
    [local hostRoleChanged:keyA];
    CHECK(Await(^BOOL{ return [link.paths.lastObject isEqual:@"/fixture/role-a.sock"]; }));
    NSTextField *custom=Field(local.computerView,@"Management socket");
    CHECK(custom);
    custom.stringValue=@"/fixture/custom.sock";
    [NSApp sendAction:@selector(connect:) to:local from:nil];
    CHECK([link.paths.lastObject isEqual:@"/fixture/custom.sock"]);
    link.push(@"Fixture",Snapshot(30,all,now+60,NO),policy,nil,NO);
    /* A role change while custom: no lookup, the custom link stays. */
    atomic_int customCalls=0;
    local.discover=Answer(keyB,@"/fixture/role-b.sock",nil,nil,&customCalls);
    [local hostRoleChanged:keyB];
    Turn();
    CHECK(atomic_load(&customCalls)==0 && [link.paths.lastObject isEqual:@"/fixture/custom.sock"] && requests.numberOfRows==2);
    /* Clearing the field and pressing Connect leaves custom mode with a fresh
     * lookup: never role A's path from before the change. */
    dispatch_semaphore_t enteredD=dispatch_semaphore_create(0), gateD=dispatch_semaphore_create(0);
    local.discover=Answer(keyB,@"/fixture/role-b.sock",enteredD,gateD,NULL);
    custom.stringValue=@"";
    [NSApp sendAction:@selector(connect:) to:local from:nil];
    CHECK(Entered(enteredD));
    CHECK([link.paths.lastObject isEqual:@""] && requests.numberOfRows==0);
    dispatch_semaphore_signal(gateD);
    CHECK(Await(^BOOL{ return [link.paths.lastObject isEqual:@"/fixture/role-b.sock"]; }));
    CHECK(![[link.paths subarrayWithRange:NSMakeRange(link.paths.count-2,2)] containsObject:@"/fixture/role-a.sock"]);
    /* Round 5 #1: A connected; the saved role becomes B outside the manager
     * and a fresh lookup (Connect, field empty) reads B but is held; the role
     * goes back to A and Identity reports A before B's answer lands. The
     * pending lookup means A can't be taken as unchanged, and B's answer
     * disagrees with the latest role, so B is never connected to. */
    custom.stringValue=@"";
    local.discover=Answer(keyA,@"/fixture/role-a.sock",nil,nil,NULL);
    [local hostRoleChanged:keyA];
    CHECK(Await(^BOOL{ return [link.paths.lastObject isEqual:@"/fixture/role-a.sock"]; }));
    dispatch_semaphore_t enteredE=dispatch_semaphore_create(0), gateE=dispatch_semaphore_create(0);
    local.discover=Answer(keyB,@"/fixture/role-b2.sock",enteredE,gateE,NULL);
    [NSApp sendAction:@selector(connect:) to:local from:nil];
    CHECK(Entered(enteredE));
    NSUInteger mark=link.paths.count;
    local.discover=Answer(keyA,@"/fixture/role-a.sock",nil,nil,NULL);
    [local hostRoleChanged:keyA];
    dispatch_semaphore_signal(gateE);
    CHECK(Await(^BOOL{ return [link.paths.lastObject isEqual:@"/fixture/role-a.sock"]; }));
    CHECK(![[link.paths subarrayWithRange:NSMakeRange(mark,link.paths.count-mark)] containsObject:@"/fixture/role-b2.sock"]);
    /* A lookup that answers with a role other than the one Identity last
     * reported is rejected, not connected. */
    local.discover=Answer(keyB,@"/fixture/role-b3.sock",nil,nil,NULL);
    [NSApp sendAction:@selector(connect:) to:local from:nil];
    CHECK(Await(^BOOL{ return LabelContains(local.computerView,@"host role changed while looking"); }));
    CHECK(![link.paths containsObject:@"/fixture/role-b3.sock"]);
    /* Round 6 #2: Try Again re-reads the saved role (B, which the lookup
     * also finds) instead of comparing with Identity's earlier A. */
    NSButton *retry=Button(local.computerView,@"Try Again");
    CHECK(retry && Visible(retry));
    CHECK(Send(retry));
    CHECK(Await(^BOOL{ return [link.paths.lastObject isEqual:@"/fixture/role-b3.sock"]; }));
    /* The role changes between Try Again's read (C) and its lookup (B): the
     * lookup disagrees with what was just read and is refused. */
    __block atomic_int step=0;
    NSArray *reads=@[@[keyC,@"/fixture/role-c2.sock"], @[keyB,@"/fixture/role-b4.sock"]];
    local.discover=^NSString *(NSString **hostKey) {
        NSUInteger index=(NSUInteger)atomic_fetch_add(&step,1); /* once: MIN may evaluate twice */
        NSArray *answer=reads[index < reads.count ? index : reads.count-1];
        *hostKey=answer[0]; return answer[1];
    };
    [NSApp sendAction:@selector(reconnect:) to:local from:nil];
    CHECK(Await(^BOOL{ return atomic_load(&step)>=2 && LabelContains(local.computerView,@"host role changed while looking"); }));
    CHECK(![link.paths containsObject:@"/fixture/role-b4.sock"] && ![link.paths containsObject:@"/fixture/role-c2.sock"]);
    local.discover=Answer(nil,nil,nil,nil,NULL); /* later role changes answer at once */
    [local hostRoleChanged:@""];
    CHECK(Await(^BOOL{ return LabelContains(local.computerView,@"doesn’t have a host role"); }));
    return YES;
}
/* B2: the Relays section's host identity field refuses secret keys too. */
static BOOL RelaysField(MDManagerApp *app, NSWindow *window) {
    [app showSection:@"Relays"];
    NSView *root=window.contentView;
    CHECK(Refuses(window, Field(root,@"Host identity"), root, @"That’s a private key"));
    return YES;
}
/* S9: a late pending-request update after the app delegate is gone is ignored. */
static BOOL LateBadge(void) {
    FixtureLink *link=[[FixtureLink alloc] initWithUpdate:nil]; link.commands=[NSMutableArray array]; link.paths=[NSMutableArray array];
    MDLocalHostPages *local=[[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) { link.push=update; return link; }];
    FixtureHosts *worker=[[FixtureHosts alloc] initWithChange:nil];
    MDHostsPage *hosts=[[MDHostsPage alloc] initWithWorker:worker];
    MDIdentityPage *identity=[[MDIdentityPage alloc] initWithHosts:hosts directory:@"/nonexistent" connect:Connect credentials:nil];
    __weak MDManagerApp *gone=nil;
    @autoreleasepool {
        MDManagerApp *app=[[MDManagerApp alloc] initWithHosts:hosts local:local identity:identity];
        NSWindow *window=[app makeMainWindow]; window.alphaValue=0;
        gone=app;
        [window close];
    }
    CHECK(gone==nil && local.pendingChanged);
    local.pendingChanged(2);
    __block int pending=3;
    [hosts close:^{ --pending; }]; [local close:^{ --pending; }]; [identity close:^{ --pending; }];
    CHECK(Await(^BOOL{ return pending==0; }));
    return YES;
}
/* Round 4 #1: a fresh app that goes straight to Relays, never opening Hosts,
 * shows the saved list; Save stays off until it has loaded, writes exactly
 * the list shown, and an empty list contacts no relay. */
static BOOL RelaysFirst(void) {
    NSString *prefs=[[@(getenv("XDG_CONFIG_HOME")) stringByAppendingPathComponent:@"metadesk"] stringByAppendingPathComponent:@"manager-relays.ini"];
    NSString *(^saved)(void)=^NSString *{ return [NSString stringWithContentsOfFile:prefs encoding:NSUTF8StringEncoding error:NULL]; };
    FixtureHosts *worker=[[FixtureHosts alloc] initWithChange:nil]; worker.state=MDHostsSynced;
    MDHostsPage *hosts=[[MDHostsPage alloc] initWithWorker:worker];
    FixtureLink *link=[[FixtureLink alloc] initWithUpdate:nil]; link.commands=[NSMutableArray array]; link.paths=[NSMutableArray array];
    MDLocalHostPages *local=[[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) { link.push=update; return link; }];
    local.discover=^NSString *(NSString **hostKey) { *hostKey=nil; return nil; };
    MDIdentityPage *identity=[[MDIdentityPage alloc] initWithHosts:hosts directory:@"/nonexistent" connect:Connect credentials:nil];
    /* Saved by MDHostsBootstrap itself, so the file is in its own format. */
    NSString *error=nil;
    CHECK([MDHostsBootstrap(@"wss://first.fixture.invalid/", &error) containsString:@"first.fixture.invalid"]);
    NSString *original=saved();
    CHECK(original.length);
    MDManagerApp *app=[[MDManagerApp alloc] initWithHosts:hosts local:local identity:identity];
    app.initialSection=@"Relays"; /* test hook: start on Relays without ever showing Hosts */
    NSWindow *window=[app makeMainWindow]; window.alphaValue=0;
    CHECK([window.title isEqual:@"Relays"] && ![[hosts valueForKey:@"started"] boolValue]);
    NSView *root=window.contentView;
    NSTextField *relays=Field(root,@"Bootstrap relays");
    NSButton *save=Button(root,@"Save and Sync");
    CHECK(relays && save && Visible(save) && !save.enabled);
    CHECK(Await(^BOOL{ return save.enabled; }));
    CHECK([relays.stringValue containsString:@"wss://first.fixture.invalid/"] && worker.starts==0);
    /* Save with the list unchanged: the same list is kept and synced. */
    [save performClick:nil];
    CHECK(Await(^BOOL{ return LabelContains(root,@"Relays saved"); }));
    CHECK([saved() isEqual:original] && worker.starts==1 && [worker.relays containsString:@"first.fixture.invalid"]);
    /* Clearing it: the empty list is saved and nothing connects. */
    relays.stringValue=@"";
    CHECK(Await(^BOOL{ return save.enabled; }));
    [save performClick:nil];
    CHECK(Await(^BOOL{ return LabelContains(root,@"Relays cleared"); }));
    CHECK(![saved() containsString:@"first.fixture.invalid"] && worker.starts==1);
    /* Leaving and returning shows the saved (empty) list again. */
    relays.stringValue=@"wss://unsaved.fixture.invalid/";
    [app showSection:@"This Computer"]; [app showSection:@"Relays"];
    CHECK(Await(^BOOL{ return save.enabled && relays.stringValue.length==0; }));
    __block int pending=3;
    [hosts close:^{ --pending; }]; [local close:^{ --pending; }]; [identity close:^{ --pending; }];
    CHECK(Await(^BOOL{ return pending==0; }));
    [window close];
    return YES;
}
/* Round 4 #4: an explicit Opening state from the worker is blocked like
 * the other first-run states, so it agrees with the panel renderState shows. */
static BOOL OpeningState(MDManagerApp *app, NSWindow *window, MDHostsPage *hosts, FixtureHosts *worker) {
    worker.state=MDHostsOpening;
    [app showSection:@"Hosts"];
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)]; }));
    [NSApp sendAction:@selector(refresh:) to:hosts from:nil];
    MDSetupPanel *panel=(MDSetupPanel *)SetupPanel(hosts.view);
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)] && Visible(panel) && [panel.titleLabel.stringValue containsString:@"Opening"]; }));
    CHECK(!Visible(Field(hosts.view,@"Host npub")));
    CHECK(![hosts canPerform:@selector(focusHostEntry:)] && !Enabled(window,@selector(connectToHost:)));
    worker.state=MDHostsSynced;
    [NSApp sendAction:@selector(refresh:) to:hosts from:nil];
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)] && Visible(Field(hosts.view,@"Host npub")); }));
    CHECK([hosts canPerform:@selector(focusHostEntry:)] && Enabled(window,@selector(connectToHost:)));
    return YES;
}
/* Round 4 #3: a client-role change made outside the manager while Hosts is
 * ready reopens the book for the new key; a label edit does not. */
static BOOL ClientRole(MDManagerApp *app, MDHostsPage *hosts, FixtureHosts *worker) {
    NSString *keyA=@"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", *keyB=@"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    NSString *saved=worker.owner;
    worker.state=MDHostsSynced; worker.owner=keyA;
    [app showSection:@"Hosts"];
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)]; }));
    [NSApp sendAction:@selector(refresh:) to:hosts from:nil];
    NSButton *review=Button(hosts.view,@"Review and Connect…");
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)] && review.enabled; }));
    NSUInteger closes=worker.closes;
    /* Same key (a label edit, or nothing changed): no reopen. */
    [hosts identityChanged:keyA];
    Turn();
    CHECK(worker.closes==closes && review.enabled);
    /* The role now names B: Hosts suspends, closes the old book, reopens. */
    worker.owner=keyB;
    [hosts identityChanged:keyB];
    CHECK(Await(^BOOL{ return worker.closes==closes+1 && [hosts canPerform:@selector(refresh:)] && review.enabled; }));
    [hosts identityChanged:keyB];
    Turn();
    CHECK(worker.closes==closes+1);
    worker.owner=saved;
    [hosts identityChanged:nil]; /* unknown: nothing to compare */
    Turn();
    CHECK(worker.closes==closes+1);
    return YES;
}
/* Round 5 #4: a client-role mismatch learned while a Hosts operation is in
 * flight suppresses that operation's review and reconciles first. */
static BOOL ClientStale(MDManagerApp *app, MDHostsPage *hosts, FixtureHosts *worker, NSWindow *window) {
    NSString *keyA=@"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", *keyB=@"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    NSString *saved=worker.owner;
    /* Round 6 #1 (targeted): from the no-client-role panel, Identity reports
     * a newly assigned role. The reload reconciliation starts belongs to the
     * new owner and must be delivered: Hosts leaves the panel, not busy. */
    worker.state=MDHostsNoClientRole; worker.owner=nil;
    [app showSection:@"Hosts"];
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)]; }));
    [NSApp sendAction:@selector(refresh:) to:hosts from:nil];
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)] && Visible(SetupPanel(hosts.view)); }));
    worker.state=MDHostsSynced; worker.owner=keyA;
    [hosts identityChanged:keyA];
    NSButton *review=Button(hosts.view,@"Review and Connect…");
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)] && review.enabled && !Visible(SetupPanel(hosts.view)); }));
    [NSApp sendAction:@selector(refresh:) to:hosts from:nil];
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)] && review.enabled; }));
    [hosts identityChanged:keyA];
    NSUInteger closes=worker.closes, resolves=worker.resolves;
    /* Hold the review lookup in the worker. */
    __block BOOL entered=NO;
    worker.entered=^{ entered=YES; };
    worker.proceed=dispatch_semaphore_create(0);
    Field(hosts.view,@"Host npub").stringValue=@KEY;
    CHECK(Send(review) && Await(^BOOL{ return entered; }) && worker.resolves==resolves+1);
    /* Identity now reports client role B; then the lookup finishes. */
    worker.owner=keyB;
    [hosts identityChanged:keyB];
    dispatch_semaphore_signal(worker.proceed);
    CHECK(Await(^BOOL{ return worker.closes==closes+1 && [hosts canPerform:@selector(refresh:)] && review.enabled; }));
    CHECK(!window.attachedSheet && !LabelContains(hosts.view,@"Checking the host"));
    worker.entered=nil; worker.proceed=nil; worker.owner=saved;
    [hosts identityChanged:nil];
    return YES;
}
/* Round 5 #2: a fresh app opens on Hosts and starts it, showing the first-run
 * panel; the section is never saved. */
static BOOL LaunchOnHosts(void) {
    for (int round=0; round<2; ++round) {
        FixtureHosts *worker=[[FixtureHosts alloc] initWithChange:nil]; worker.state=MDHostsNoClientRole;
        MDHostsPage *hosts=[[MDHostsPage alloc] initWithWorker:worker];
        hosts.accountCount=^NSUInteger{ return 0; };
        FixtureLink *link=[[FixtureLink alloc] initWithUpdate:nil]; link.commands=[NSMutableArray array]; link.paths=[NSMutableArray array];
        MDLocalHostPages *local=[[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) { link.push=update; return link; }];
        local.discover=^NSString *(NSString **hostKey) { *hostKey=nil; return nil; };
        MDIdentityPage *identity=[[MDIdentityPage alloc] initWithHosts:hosts directory:@"/nonexistent" connect:Connect credentials:nil];
        MDManagerApp *app=[[MDManagerApp alloc] initWithHosts:hosts local:local identity:identity];
        NSWindow *window=[app makeMainWindow]; window.alphaValue=0;
        CHECK([window.title isEqual:@"Hosts"] && [[hosts valueForKey:@"started"] boolValue]);
        CHECK(!link.paths.count); /* This Computer was not started */
        MDSetupPanel *panel=(MDSetupPanel *)SetupPanel(hosts.view);
        CHECK(Await(^BOOL{ return Visible(panel) && [panel.titleLabel.stringValue isEqual:@"Add Your Signer Account"]; }));
        [app showSection:@"Access"]; /* a second launch must not start here */
        __block int pending=3;
        [hosts close:^{ --pending; }]; [local close:^{ --pending; }]; [identity close:^{ --pending; }];
        CHECK(Await(^BOOL{ return pending==0; }));
        [window close];
    }
    return YES;
}
/* Round 5 #3: from another section, Connect to Host (⌘K) and the File-menu
 * host commands go to Hosts and start it, then apply the page's own checks. */
static BOOL HostCommands(void) {
    FixtureHosts *worker=[[FixtureHosts alloc] initWithChange:nil]; worker.state=MDHostsNoClientRole;
    MDHostsPage *hosts=[[MDHostsPage alloc] initWithWorker:worker];
    hosts.accountCount=^NSUInteger{ return 0; };
    FixtureLink *link=[[FixtureLink alloc] initWithUpdate:nil]; link.commands=[NSMutableArray array]; link.paths=[NSMutableArray array];
    MDLocalHostPages *local=[[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) { link.push=update; return link; }];
    local.discover=^NSString *(NSString **hostKey) { *hostKey=nil; return nil; };
    MDIdentityPage *identity=[[MDIdentityPage alloc] initWithHosts:hosts directory:@"/nonexistent" connect:Connect credentials:nil];
    MDManagerApp *app=[[MDManagerApp alloc] initWithHosts:hosts local:local identity:identity];
    app.initialSection=@"This Computer";
    NSWindow *window=[app makeMainWindow]; window.alphaValue=0;
    id delegate=NSApp.delegate; NSApp.delegate=app;
    CHECK(![[hosts valueForKey:@"started"] boolValue]);
    /* ⌘K starts Hosts and lands on its first-run panel. */
    CHECK(Choose(window,@selector(connectToHost:)));
    CHECK([window.title isEqual:@"Hosts"] && [[hosts valueForKey:@"started"] boolValue]);
    MDSetupPanel *panel=(MDSetupPanel *)SetupPanel(hosts.view);
    CHECK(Await(^BOOL{ return Visible(panel) && [hosts canPerform:@selector(refresh:)]; }));
    /* Import from another section: goes to Hosts, where the page's own
     * check (nothing to import into yet) stops it. On Hosts it's disabled. */
    [app showSection:@"Access"];
    CHECK(Enabled(window,@selector(importHosts:)) && Enabled(window,@selector(exportHosts:)) && Enabled(window,@selector(scanQR:)));
    CHECK(Choose(window,@selector(importHosts:)));
    Turn();
    CHECK([window.title isEqual:@"Hosts"] && !window.attachedSheet);
    CHECK(!Enabled(window,@selector(importHosts:)) && !Enabled(window,@selector(connectToHost:)));
    /* Ready: ⌘K from another section focuses the Host field. */
    worker.state=MDHostsSynced;
    [NSApp sendAction:@selector(refresh:) to:hosts from:nil];
    NSTextField *address=Field(hosts.view,@"Host npub");
    CHECK(Await(^BOOL{ return [hosts canPerform:@selector(refresh:)] && Visible(address); }));
    [app showSection:@"Identity"];
    [window makeFirstResponder:nil];
    CHECK(Choose(window,@selector(connectToHost:)));
    CHECK([window.title isEqual:@"Hosts"] && address.currentEditor && window.firstResponder==address.currentEditor);
    /* A sheet on the main window, or a client-role edit, disables them all. */
    [app showSection:@"Access"];
    NSAlert *alert=[NSAlert new]; alert.messageText=@"Fixture"; [alert addButtonWithTitle:@"OK"];
    [alert beginSheetModalForWindow:window completionHandler:nil];
    CHECK(Await(^BOOL{ return window.attachedSheet!=nil; }));
    for (NSString *name in @[@"connectToHost:", @"scanQR:", @"importHosts:", @"exportHosts:"]) CHECK(!Enabled(window,NSSelectorFromString(name)));
    CHECK(EndSheet(window,NSAlertFirstButtonReturn));
    __block BOOL drained=NO;
    [hosts suspendForRoleEdit:^{ drained=YES; }];
    CHECK(Await(^BOOL{ return drained; }));
    for (NSString *name in @[@"connectToHost:", @"scanQR:", @"importHosts:", @"exportHosts:"]) CHECK(!Enabled(window,NSSelectorFromString(name)));
    [app performSelector:@selector(connectToHost:) withObject:nil];
    CHECK([window.title isEqual:@"Access"]);
    [hosts resumeAfterRoleEdit];
    NSApp.delegate=delegate; KeyWindowOverride=nil;
    __block int pending=3;
    [hosts close:^{ --pending; }]; [local close:^{ --pending; }]; [identity close:^{ --pending; }];
    CHECK(Await(^BOOL{ return pending==0; }));
    [window close];
    return YES;
}
/* ---- Round 6: the new user's journey, end to end, with fixtures ----
 * JourneyHosts derives its state the way the real worker does, from the
 * fixture identity directory (accounts and client role), the signer's
 * availability and whether relays were started; JourneyConnect stands in for
 * a signer that proves whichever key it is asked for. */
@interface JourneyHosts : MDHostsWorker
@property(copy) NSString *directory;
@property(atomic) BOOL signerDown;
@property(atomic) NSUInteger closes;
@property(atomic, copy) dispatch_block_t entered;
@property(atomic, strong) dispatch_semaphore_t proceed;
@property(atomic) NSUInteger opens;
/* Holds: closeGate blocks close; openGate blocks a reload after it has read
 * the client role. Each signals its flag on the main queue first. */
@property(atomic, strong) dispatch_semaphore_t closeGate, openGate;
@property(atomic) BOOL closeHeld, openHeld;
@property(atomic) NSUInteger starts;
@property(atomic) MDHostsState syncState;
@end
static void Hold(JourneyHosts *w, dispatch_semaphore_t gate, BOOL close) {
    if (!gate) return;
    dispatch_async(dispatch_get_main_queue(), ^{ if (close) w.closeHeld=YES; else w.openHeld=YES; });
    dispatch_semaphore_wait(gate, dispatch_time(DISPATCH_TIME_NOW, 8*NSEC_PER_SEC));
}
@implementation JourneyHosts {
    NSString *_owner, *_relays; MDHostsState _blocker; /* worker queue only */
}
- (BOOL)openDirectory:(NSString *)directory error:(NSString **)error {
    (void)directory; (void)error;
    NSString *client=nil;
    for (NSDictionary *a in MDIdentityAccounts(self.directory,NULL)) if ([a[@"client"] boolValue]) client=a[@"key"];
    ++self.opens;
    Hold(self, self.openGate, NO);
    NSString *owner=client && !self.signerDown ? client : nil;
    if (![owner isEqual:_owner]) _relays=nil;
    _owner=owner;
    _blocker=!client ? MDHostsNoClientRole : self.signerDown ? MDHostsSignerUnavailable : MDHostsSynced;
    return _owner != nil;
}
- (BOOL)startRelays:(NSString *)urls { ++self.starts; if (_owner) _relays=urls; return YES; }
- (MDHostsBook *)snapshot {
    FixtureBook *b=[FixtureBook new];
    b.fixtureState=_blocker!=MDHostsSynced ? _blocker : _relays.length ? (self.syncState ?: MDHostsSynced) : MDHostsNoRelays;
    b.fixtureOwner=_owner; return b;
}
- (MDHostReview *)resolve:(NSString *)value route:(NSString *)route cancelled:(bool (*)(void *))cancelled data:(void *)data error:(NSString **)error {
    (void)value; (void)route; (void)cancelled; (void)data; (void)error;
    dispatch_block_t entered=self.entered; dispatch_semaphore_t proceed=self.proceed;
    if (entered) { dispatch_async(dispatch_get_main_queue(),entered); dispatch_semaphore_wait(proceed,dispatch_time(DISPATCH_TIME_NOW,8*NSEC_PER_SEC)); }
    return [FixtureReview new];
}
- (void)close { Hold(self, self.closeGate, YES); ++self.closes; _owner=nil; _relays=nil; }
@end
static int JourneyKey(MdSigner *s,char **key) { *key=strdup((const char *)s->backend); return *key?0:MD_SIGNER_ERR_INTERNAL; }
static void JourneyDrop(MdSigner *s) { free(s->backend); }
/* When set, an in-app bind waits here (in Identity's worker) until released. */
static dispatch_semaphore_t JourneyBindGate;
static atomic_int JourneyBindEntered;
static int JourneyConnect(MdSignerType backend,const char *key,MdCredentialStore *store,MdSigner **out) {
    (void)store;
    dispatch_semaphore_t gate=JourneyBindGate;
    if (gate) { atomic_store(&JourneyBindEntered,1); dispatch_semaphore_wait(gate,dispatch_time(DISPATCH_TIME_NOW,8*NSEC_PER_SEC)); }
    static const MdSignerOps ops={.get_pubkey=JourneyKey,.destroy=JourneyDrop};
    char *copy=strdup(key);
    *out=copy ? md_signer_create_custom(backend,&ops,copy) : NULL;
    if (!*out) { free(copy); return MD_SIGNER_ERR_INTERNAL; }
    return 0;
}
static MdCredentialStore *JourneyStore(void) {
    static const MdCredentialStoreOps ops={.get=GetPair,.put=PutPair,.remove=RemovePair};
    return md_credential_store_custom(&ops,NULL);
}
@interface Journey : NSObject
@property(strong) MDManagerApp *app; @property(strong) MDHostsPage *hosts; @property(strong) JourneyHosts *worker;
@property(strong) MDIdentityPage *identity; @property(strong) MDLocalHostPages *local; @property(strong) NSWindow *window;
@property(copy) NSString *directory; @property MdSignerType backend;
@end
@implementation Journey
@end
static Journey *OpenJourney(NSString *root, NSString *name, MdSignerType backend, BOOL signerDown) {
    Journey *j=[Journey new]; j.backend=backend;
    j.directory=[root stringByAppendingPathComponent:name];
    [NSFileManager.defaultManager createDirectoryAtPath:j.directory withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:NULL];
    j.worker=[[JourneyHosts alloc] initWithChange:nil]; j.worker.directory=j.directory; j.worker.signerDown=signerDown;
    j.hosts=[[MDHostsPage alloc] initWithWorker:j.worker];
    NSString *directory=j.directory;
    j.hosts.accountCount=^NSUInteger{ return MDIdentityAccounts(directory,NULL).count; };
    FixtureLink *link=[[FixtureLink alloc] initWithUpdate:nil]; link.commands=[NSMutableArray array]; link.paths=[NSMutableArray array];
    j.local=[[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) { link.push=update; return link; }];
    j.local.discover=^NSString *(NSString **hostKey) { *hostKey=nil; return nil; };
    j.identity=[[MDIdentityPage alloc] initWithHosts:j.hosts directory:directory connect:JourneyConnect credentials:^{ return JourneyStore(); }];
    j.app=[[FixtureApp alloc] initWithHosts:j.hosts local:j.local identity:j.identity];
    j.window=[j.app makeMainWindow]; j.window.alphaValue=0;
    KeyWindowOverride=j.window;
    return j;
}
static BOOL CloseJourney(Journey *j) {
    __block int pending=3;
    [j.hosts close:^{ --pending; }]; [j.local close:^{ --pending; }]; [j.identity close:^{ --pending; }];
    CHECK(Await(^BOOL{ return pending==0; }));
    [j.window close]; KeyWindowOverride=nil;
    NSString *error=nil; MDHostsBootstrap(@"",&error);
    return YES;
}
/* One explicit deadline per journey wait (6 s: a step may chain several
 * worker round trips). On timeout it names the step and its line. */
static BOOL Within(const char *step, int line, BOOL (^ready)(void)) {
    CFAbsoluteTime limit=CFAbsoluteTimeGetCurrent()+6;
    while (!ready()) {
        CFTimeInterval left=limit-CFAbsoluteTimeGetCurrent();
        if (left<=0) { fprintf(stderr,"TIMEOUT after 6 s (test_pages.m:%d): %s\n",line,step); return NO; }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,left,true);
    }
    return YES;
}
#define Eventually(...) Within(#__VA_ARGS__, __LINE__, __VA_ARGS__)
/* Hosts is showing the setup panel titled title, idle, its next-step button
 * enabled, and the connect form hidden. */
static BOOL ShowsPanel(Journey *j, NSString *title) {
    [j.app showSection:@"Hosts"];
    return Within([NSString stringWithFormat:@"Hosts idle on the panel “%@”", title].UTF8String, __LINE__, ^BOOL{
        MDSetupPanel *panel=(MDSetupPanel *)SetupPanel(j.hosts.view);
        return [j.hosts canPerform:@selector(refresh:)] && Visible(panel) && [panel.titleLabel.stringValue isEqual:title] &&
            panel.primary.enabled && !Visible(Field(j.hosts.view,@"Host npub"));
    });
}
/* Hosts is showing its page (no panel), idle, with the Host field enabled;
 * a usable cache enables key review; ready checks the name-based sync gate. */
static BOOL ShowsPage(Journey *j, NSString *banner, BOOL ready) {
    [j.app showSection:@"Hosts"];
    return Within([NSString stringWithFormat:@"Hosts idle on its page (banner “%@”, ready %d)", banner ?: @"none", ready].UTF8String, __LINE__, ^BOOL{
        NSView *v=j.hosts.view; NSTextField *address=Field(v,@"Host npub");
        return [j.hosts canPerform:@selector(refresh:)] && !Visible(SetupPanel(v)) && Visible(address) && address.enabled &&
            [Button(v,@"Review and Connect…") isEnabled] && [[[j.hosts valueForKey:@"book"] valueForKey:@"canReview"] boolValue]==ready && (banner ? LabelContains(v,banner) : !LabelContains(v,@"Choose Relays"));
    });
}
static BOOL IdentityIdle(Journey *j) {
    return Eventually(^BOOL{ return [j.identity validateMenuItem:MenuItem(NSApp.mainMenu,@selector(refresh:))]; });
}
/* From the first-run panel: its button opens Add Signer Account; add KEY. */
static BOOL AddFirstAccount(Journey *j) {
    CHECK(ShowsPanel(j,@"Add Your Signer Account"));
    MDSetupPanel *panel=(MDSetupPanel *)SetupPanel(j.hosts.view);
    [panel.primary performClick:nil];
    CHECK(Eventually(^BOOL{ return [j.window.title isEqual:@"Identity"] && j.window.attachedSheet!=nil; }));
    NSWindow *sheet=j.window.attachedSheet; NSView *form=sheet.contentView;
    NSPopUpButton *picker=(NSPopUpButton *)Find(form,^BOOL(NSView *v){ return [v isKindOfClass:NSPopUpButton.class]; });
    NSTextField *key=Field(form,@"Public key"), *label=Field(form,@"Public label");
    CHECK(picker && key && label && [picker selectItemWithTag:j.backend]);
    CHECK([sheet makeFirstResponder:key]);
    [(NSTextView *)key.currentEditor insertText:@KEY replacementRange:NSMakeRange(0,0)];
    label.stringValue=@"Journey";
    NSButton *add=Button(form,@"Add Account");
    CHECK(add.enabled);
    [add performClick:nil];
    CHECK(Eventually(^BOOL{ return j.window.attachedSheet==nil && LabelContains(j.identity.view,@"Account added"); }));
    CHECK(IdentityIdle(j));
    /* Hosts moves on to choosing an account for connecting. */
    CHECK(ShowsPanel(j,@"Choose an Account for Connecting"));
    return YES;
}
/* Relays are saved on the Relays page; Hosts starts sync. */
static BOOL SaveJourneyRelays(Journey *j) {
    [j.app showSection:@"Relays"];
    NSView *root=j.window.contentView;
    NSTextField *relays=Field(root,@"Bootstrap relays"); NSButton *save=Button(root,@"Save and Sync");
    CHECK(Eventually(^BOOL{ return save.enabled; }));
    relays.stringValue=@"wss://journey.fixture.invalid/";
    [save performClick:nil];
    CHECK(Eventually(^BOOL{ return LabelContains(root,@"Relays saved"); }));
    return YES;
}
static NSDictionary *AccountWithKey(Journey *j, NSString *key) {
    for (NSDictionary *a in MDIdentityAccounts(j.directory,NULL)) if ([a[@"key"] isEqual:key]) return a;
    return nil;
}
static NSString *ClientID(Journey *j) {
    for (NSDictionary *a in MDIdentityAccounts(j.directory,NULL)) if ([a[@"client"] boolValue]) return a[@"id"];
    return @"";
}
/* Another program assigns the client role (or unassigns it with key nil). */
static BOOL BindOutside(Journey *j, NSString *key) {
    MdCredentialStore *store=JourneyStore();
    NSString *error=MDIdentityBind(j.directory,store,MD_ACCOUNT_CLIENT,key ? AccountWithKey(j,key) : nil,ClientID(j),NO,JourneyConnect,NULL,NULL,NULL);
    md_credential_store_destroy(store);
    CHECK(!error.length);
    return YES;
}
/* Identity's View > Refresh picks the change up. */
static BOOL RefreshIdentity(Journey *j, NSString *expected) {
    CHECK(IdentityIdle(j));
    [j.identity refresh:nil]; /* busy from here until the reload completes */
    CHECK(Eventually(^BOOL{ return [j.identity validateMenuItem:MenuItem(NSApp.mainMenu,@selector(refresh:))] && [j.identity.clientRoleKey isEqual:expected]; }));
    return YES;
}
/* a. First run entirely in the app. */
static BOOL JourneyInApp(NSString *root, MdSignerType backend) {
    Journey *j=OpenJourney(root,@"journey-in-app",backend,NO);
    CHECK([j.window.title isEqual:@"Hosts"]);
    CHECK(AddFirstAccount(j));
    /* The role is assigned in Identity through the normal binding. */
    [j.app showSection:@"Identity"];
    NSButton *bind=Button(j.identity.view,@"Use for Client");
    CHECK(Eventually(^BOOL{ return bind.enabled; }));
    CHECK(Send(bind));
    CHECK(Eventually(^BOOL{ return LabelContains(j.identity.view,@"Client role saved"); }) && IdentityIdle(j));
    /* Hosts leaves the panel: no relays yet. */
    CHECK(ShowsPage(j,@"Choose Relays to Sync Your Hosts",NO) && j.worker.starts==0);
    j.worker.syncState=MDHostsSyncing;
    CHECK(SaveJourneyRelays(j) && ShowsPage(j,@"Syncing Your Host List",NO) && j.worker.starts==1);
    NSView *view=j.hosts.view;
    NSTextField *address=Field(view,@"Host npub");
    NSButton *review=Button(view,@"Review and Connect…");
    address.stringValue=@"desk@example.invalid";
    CHECK(Send(review) && LabelContains(view,@"before reviewing a NIP-05 name") && !j.window.attachedSheet);
    address.stringValue=@KEY;
    CHECK(Send(review) && Eventually(^BOOL{ return j.window.attachedSheet!=nil; }));
    CHECK(Button(j.window.attachedSheet.contentView,@"Connect").enabled &&
        !Button(j.window.attachedSheet.contentView,@"Save Host").enabled);
    CHECK(EndSheet(j.window,NSAlertFirstButtonReturn));
    return CloseJourney(j);
}
/* b. The same, with the role assigned outside the app; then c. from ready. */
static BOOL JourneyOutside(NSString *root, MdSignerType backend) {
    Journey *j=OpenJourney(root,@"journey-outside",backend,NO);
    CHECK(AddFirstAccount(j));
    CHECK(BindOutside(j,@KEY));
    CHECK(ShowsPanel(j,@"Choose an Account for Connecting")); /* not yet picked up */
    CHECK(RefreshIdentity(j,@KEY));
    CHECK(ShowsPage(j,@"Choose Relays to Sync Your Hosts",NO) && j.worker.starts==0);
    CHECK(SaveJourneyRelays(j) && ShowsPage(j,nil,YES));

    /* c1. Switched in the app to a second account: Hosts reopens for it and
     * starts sync with the existing relay settings. */
    CHECK(!MDIdentityAdd(j.directory,backend,@OTHER,@"Second").length);
    CHECK(RefreshIdentity(j,@KEY));
    [j.app showSection:@"Identity"];
    NSTableView *accounts=(NSTableView *)Find(j.identity.view,^BOOL(NSView *v){ return [v isKindOfClass:NSTableView.class] && [v.accessibilityLabel isEqual:@"Signer accounts"]; });
    NSUInteger row=[MDIdentityAccounts(j.directory,NULL) indexOfObjectPassingTest:^BOOL(NSDictionary *a, NSUInteger i, BOOL *stop) { (void)i; (void)stop; return [a[@"key"] isEqual:@OTHER]; }];
    CHECK(row!=NSNotFound);
    [accounts selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    NSButton *bind=Button(j.identity.view,@"Use for Client");
    NSUInteger closes=j.worker.closes;
    CHECK(Eventually(^BOOL{ return bind.enabled; }) && Send(bind));
    CHECK(Eventually(^BOOL{ return [j.identity.clientRoleKey isEqual:@OTHER]; }) && IdentityIdle(j));
    CHECK(ShowsPage(j,nil,YES) && j.worker.closes==closes+1);

    /* c2. Switched outside the app while Hosts is idle. */
    closes=j.worker.closes;
    CHECK(BindOutside(j,@KEY) && RefreshIdentity(j,@KEY));
    CHECK(ShowsPage(j,nil,YES) && j.worker.closes==closes+1);

    /* c3. Switched outside while a Hosts operation is held in flight: the
     * old owner's review never appears; Hosts reopens for the new owner. */
    __block BOOL entered=NO;
    j.worker.proceed=dispatch_semaphore_create(0);
    j.worker.entered=^{ entered=YES; };
    Field(j.hosts.view,@"Host npub").stringValue=@KEY;
    closes=j.worker.closes;
    CHECK(Send(Button(j.hosts.view,@"Review and Connect…")) && Eventually(^BOOL{ return entered; }));
    CHECK(BindOutside(j,@OTHER) && RefreshIdentity(j,@OTHER));
    dispatch_semaphore_signal(j.worker.proceed);
    j.worker.entered=nil;
    CHECK(ShowsPage(j,nil,YES) && j.worker.closes==closes+1);
    CHECK(!j.window.attachedSheet && !LabelContains(j.hosts.view,@"Checking the host"));

    /* c4. Unassigned in the app: back to choosing an account. */
    [j.app showSection:@"Identity"];
    CHECK(IdentityIdle(j));
    [j.identity performSelector:@selector(unbindClient:) withObject:nil];
    CHECK(Eventually(^BOOL{ return [j.identity.clientRoleKey isEqual:@""]; }) && IdentityIdle(j));
    CHECK(ShowsPanel(j,@"Choose an Account for Connecting"));
    return CloseJourney(j);
}
static BOOL SelectAccount(Journey *j, NSString *key) {
    [j.app showSection:@"Identity"];
    CHECK(IdentityIdle(j));
    NSTableView *accounts=(NSTableView *)Find(j.identity.view,^BOOL(NSView *v){ return [v isKindOfClass:NSTableView.class] && [v.accessibilityLabel isEqual:@"Signer accounts"]; });
    NSUInteger row=[MDIdentityAccounts(j.directory,NULL) indexOfObjectPassingTest:^BOOL(NSDictionary *a, NSUInteger i, BOOL *stop) { (void)i; (void)stop; return [a[@"key"] isEqual:key]; }];
    CHECK(row!=NSNotFound && accounts.numberOfRows>(NSInteger)row);
    [accounts selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    return YES;
}
static NSString *BookOwner(Journey *j) { return [[j.hosts valueForKey:@"book"] owner]; }
/* Hosts is suspended: the switching panel, no reload, refresh and the
 * File-menu host commands disabled, Relays' Save disabled. */
static BOOL Suspended(Journey *j, NSUInteger opens) {
    [j.app showSection:@"Hosts"];
    MDSetupPanel *panel=(MDSetupPanel *)SetupPanel(j.hosts.view);
    CHECK(Visible(panel) && [panel.titleLabel.stringValue isEqual:@"Switching Client Account…"] && j.worker.opens==opens);
    CHECK(![j.hosts canPerform:@selector(refresh:)] && !Visible(Field(j.hosts.view,@"Host npub")));
    for (NSString *name in @[@"connectToHost:", @"scanQR:", @"importHosts:", @"exportHosts:", @"refresh:"]) CHECK(!Enabled(j.window,NSSelectorFromString(name)));
    [j.app showSection:@"Relays"];
    CHECK(![Button(j.window.contentView,@"Save and Sync") isEnabled]);
    [j.app showSection:@"Hosts"];
    return YES;
}
/* Round 7: reconciliation overlapping other changes. Starts ready as KEY
 * with OTHER also saved. */
static BOOL JourneyReconcile(NSString *root, MdSignerType backend) {
    NSString *directory=[root stringByAppendingPathComponent:@"journey-reconcile"];
    [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:NULL];
    CHECK(!MDIdentityAdd(directory,backend,@KEY,@"First").length && !MDIdentityAdd(directory,backend,@OTHER,@"Second").length);
    MdCredentialStore *store=JourneyStore();
    NSDictionary *first=nil; for (NSDictionary *a in MDIdentityAccounts(directory,NULL)) if ([a[@"key"] isEqual:@KEY]) first=a;
    CHECK(!MDIdentityBind(directory,store,MD_ACCOUNT_CLIENT,first,@"",NO,JourneyConnect,NULL,NULL,NULL).length);
    md_credential_store_destroy(store);
    NSString *error=nil;
    CHECK(MDHostsBootstrap(@"wss://journey.fixture.invalid/",&error).length);
    Journey *j=OpenJourney(root,@"journey-reconcile",backend,NO);
    CHECK(ShowsPage(j,nil,YES) && j.worker.starts==1);
    CHECK(RefreshIdentity(j,@KEY));

    /* 1. Overlapping suspensions. An external change to OTHER is picked up by
     * an Identity refresh; reconciliation suspends Hosts, and its close is
     * held. Meanwhile the user binds KEY in the app, and that bind is held at
     * the signer. Reconciliation's resume arrives first: Hosts must stay
     * suspended until the bind ends, then reopen once, for KEY. */
    NSUInteger opens=j.worker.opens, closes=j.worker.closes;
    j.worker.closeHeld=NO; j.worker.closeGate=dispatch_semaphore_create(0);
    CHECK(BindOutside(j,@OTHER) && RefreshIdentity(j,@OTHER));
    CHECK(Eventually(^BOOL{ return j.worker.closeHeld; }));
    CHECK(SelectAccount(j,@KEY));
    NSButton *bind=Button(j.identity.view,@"Use for Client");
    atomic_store(&JourneyBindEntered,0); JourneyBindGate=dispatch_semaphore_create(0);
    CHECK(bind.enabled && Send(bind));
    dispatch_semaphore_t gate=j.worker.closeGate; j.worker.closeGate=nil;
    dispatch_semaphore_signal(gate);
    /* The bind's work starts only after its own suspension's ready, which
     * is queued behind reconciliation's: once it has entered the signer,
     * reconciliation's resume has already run. */
    CHECK(Eventually(^BOOL{ return atomic_load(&JourneyBindEntered)!=0; }));
    for (int i=0;i<5;++i) Turn();
    CHECK(Suspended(j,opens));
    dispatch_semaphore_t signer=JourneyBindGate; JourneyBindGate=nil;
    dispatch_semaphore_signal(signer);
    CHECK(Eventually(^BOOL{ return [j.identity.clientRoleKey isEqual:@KEY]; }) && IdentityIdle(j));
    CHECK(ShowsPage(j,nil,YES));
    CHECK(j.worker.opens==opens+1 && j.worker.closes==closes+1 && [BookOwner(j) isEqual:@KEY]);

    /* 2. A role change reported during reconciliation's own reload: the
     * reload (which read OTHER) is held; Identity then reports KEY again.
     * The held reload is delivered for OTHER, and reconciliation follows up
     * with one more suspend and reopen, ending on KEY. */
    opens=j.worker.opens; closes=j.worker.closes;
    j.worker.openHeld=NO; j.worker.openGate=dispatch_semaphore_create(0);
    CHECK(BindOutside(j,@OTHER) && RefreshIdentity(j,@OTHER));
    CHECK(Eventually(^BOOL{ return j.worker.openHeld; }));
    CHECK(BindOutside(j,@KEY) && RefreshIdentity(j,@KEY));
    gate=j.worker.openGate; j.worker.openGate=nil;
    dispatch_semaphore_signal(gate);
    CHECK(ShowsPage(j,nil,YES));
    CHECK(Eventually(^BOOL{ return [BookOwner(j) isEqual:@KEY] && [j.hosts canPerform:@selector(refresh:)]; }));
    CHECK(j.worker.closes==closes+2 && j.worker.opens==opens+2);

    /* 3. Quit during reconciliation: the close is held when Quit arrives.
     * Termination waits, then completes once; Hosts doesn't reopen. */
    opens=j.worker.opens;
    j.worker.closeHeld=NO; j.worker.closeGate=dispatch_semaphore_create(0);
    CHECK(BindOutside(j,@OTHER) && RefreshIdentity(j,@OTHER));
    CHECK(Eventually(^BOOL{ return j.worker.closeHeld; }));
    FixtureApp *app=(FixtureApp *)j.app;
    CHECK([app applicationShouldTerminate:NSApp]==NSTerminateLater && !app.replies);
    gate=j.worker.closeGate; j.worker.closeGate=nil;
    dispatch_semaphore_signal(gate);
    CHECK(Eventually(^BOOL{ return app.replies==1; }));
    for (int i=0;i<5;++i) Turn();
    CHECK(app.replies==1 && j.worker.opens==opens && [app applicationShouldTerminate:NSApp]==NSTerminateNow);
    [j.window close]; KeyWindowOverride=nil;
    MDHostsBootstrap(@"",&error);
    return YES;
}
/* d. Signer unavailable, then Try Again once it is available: ready. */
static BOOL JourneySigner(NSString *root, MdSignerType backend) {
    NSString *directory=[root stringByAppendingPathComponent:@"journey-signer"];
    [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:NULL];
    CHECK(!MDIdentityAdd(directory,backend,@KEY,@"Journey").length);
    MdCredentialStore *store=JourneyStore();
    CHECK(!MDIdentityBind(directory,store,MD_ACCOUNT_CLIENT,MDIdentityAccounts(directory,NULL).firstObject,@"",NO,JourneyConnect,NULL,NULL,NULL).length);
    md_credential_store_destroy(store);
    NSString *error=nil;
    CHECK(MDHostsBootstrap(@"wss://journey.fixture.invalid/",&error).length);
    Journey *j=OpenJourney(root,@"journey-signer",backend,YES);
    CHECK(ShowsPanel(j,@"Your Signer Isn’t Available") && j.worker.starts==0);
    MDSetupPanel *panel=(MDSetupPanel *)SetupPanel(j.hosts.view);
    CHECK([panel.primary.title isEqual:@"Try Again"]);
    j.worker.signerDown=NO;
    [panel.primary performClick:nil];
    CHECK(ShowsPage(j,nil,YES) && j.worker.starts==1);
    return CloseJourney(j);
}
static BOOL Journeys(NSString *directory, MdSignerType backend) {
    if (!MDIdentitySupportsBackend(backend)) { puts("SKIP: no compiled external signer adapter for the journey fixtures"); return YES; }
    CHECK(JourneyInApp(directory,backend));
    CHECK(JourneyOutside(directory,backend));
    CHECK(JourneySigner(directory,backend));
    CHECK(JourneyReconcile(directory,backend));
    return YES;
}
static MdSignerType JourneyBackend(void) {
    return MDIdentitySupportsBackend(MD_SIGNER_NIP46) ? MD_SIGNER_NIP46 : MDIdentitySupportsBackend(MD_SIGNER_NIP5F) ? MD_SIGNER_NIP5F : MD_SIGNER_NIP55L;
}
static BOOL RunJourneys(NSString *directory) {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [MDManagerApp installMainMenu:NSApp];
    [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:NULL];
    return Journeys(directory,JourneyBackend());
}
static BOOL Run(NSString *directory) {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    FixtureHosts *worker=[[FixtureHosts alloc] initWithChange:nil];
    MDHostsPage *hosts=[[MDHostsPage alloc] initWithWorker:worker];
    worker.state=MDHostsSynced;
    FixtureLink *link=[[FixtureLink alloc] initWithUpdate:nil]; link.commands=[NSMutableArray array]; link.paths=[NSMutableArray array];
    MDLocalHostPages *access=[[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) {
        link.push=update; return link;
    }];
    access.discover=^NSString *(NSString **hostKey) { *hostKey=nil; return nil; };
    FixtureIdentity *identity=[[FixtureIdentity alloc] initWithHosts:hosts directory:directory connect:Connect credentials:^{
        static const MdCredentialStoreOps ops={.get=GetPair,.put=PutPair,.remove=RemovePair};
        return md_credential_store_custom(&ops,NULL);
    }];
    /* The real main window and controller hierarchy, never ordered on screen. */
    FixtureApp *app=[[FixtureApp alloc] initWithHosts:hosts local:access identity:identity];
    app.initialSection=@"This Computer"; /* BeforeActivation needs Hosts unstarted */
    NSWindow *window=[app makeMainWindow];
    window.alphaValue=0;
    KeyWindowOverride=window;
    NSApp.delegate=app; /* as in production: app-level commands resolve here */
    CHECK(Menus());
    CHECK(BeforeActivation(hosts));
    CHECK(Persistence(app,window));
    [app showSection:@"Hosts"];
    CHECK(Hosts(hosts,worker,window));
    CHECK(Editing(app,window,hosts));
    CHECK(Commands(app,window,hosts));
    CHECK(SearchAndSheets(app,window));
    CHECK(Onboarding(app,window,hosts,worker,identity,directory));
    [app showSection:@"Access"];
    CHECK(Access(access,link,window));
    CHECK(ComputerRole(app,access,link,window,identity));
    CHECK(ClientRole(app,hosts,worker));
    CHECK(OpeningState(app,window,hosts,worker));
    CHECK(ClientStale(app,hosts,worker,window));
    CHECK(RelaysField(app,window));
    CHECK(RelaysSave(app,window));
    MdSignerType backend=MDIdentitySupportsBackend(MD_SIGNER_NIP46) ? MD_SIGNER_NIP46 :
        MDIdentitySupportsBackend(MD_SIGNER_NIP5F) ? MD_SIGNER_NIP5F : MD_SIGNER_NIP55L;
    [app showSection:@"Identity"];
    if (MDIdentitySupportsBackend(backend)) CHECK(Identity(identity,directory,backend));
    else puts("SKIP: no compiled external signer adapter for the Identity action fixture");
    CHECK(UnbuiltSigner(identity,directory));
    [app showSection:@"Hosts"];
    CHECK(Quit(app,hosts,worker,link,window));
    KeyWindowOverride=nil;
    NSApp.delegate=nil;
    [window close];
    CHECK(LateBadge());
    CHECK(RelaysFirst());
    CHECK(LaunchOnHosts());
    CHECK(HostCommands());
    CHECK(Journeys(directory,backend));
    return YES;
}
int main(int argc,char **argv) {
    @autoreleasepool {
        MD_CHECK(argc==2 || (argc==3 && !strcmp(argv[2],"journeys")));
        CFDictionaryRef session=CGSessionCopyCurrentDictionary();
        if (!session || CGMainDisplayID()==kCGNullDirectDisplay) {
            if (session) CFRelease(session);
            puts("SKIP: no WindowServer session for the AppKit controller fixture");
            return 77;
        }
        CFRelease(session);
        [TestApplication sharedApplication];
        if (argc==2) Run(@(argv[1]));
        else if (argc==3) RunJourneys(@(argv[1])); /* the new-user journeys alone (mutation checks) */
        return md_test_report();
    }
}
