#import "manager_app.h"
#import "hosts_page.h"
#import "hosts_model.h"
#import "access_page.h"
#import "identity_page.h"
#import "main_window.h"
#import "ui.h"
#include "md_test.h"
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
static BOOL MenuHas(NSMenu *menu, SEL action) {
    for (NSMenuItem *item in menu.itemArray) if (item.action == action || (item.submenu && MenuHas(item.submenu, action))) return YES;
    return NO;
}

/* Edit menu commands reach the connect field's editor through the real
 * responder chain of the real main window (S10). */
static BOOL Editing(MDManagerApp *app, NSWindow *window, MDHostsPage *page) {
    [app showSection:@"Hosts"];
    NSTextField *address = Field(page.view, @"Host npub");
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
    NSMenu *ownMenu = click ? [editor menuForEvent:click] : nil;
    CHECK(ownMenu != nil);
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
    [editor insertText:@"abc" replacementRange:editor.selectedRange]; Turn();
    for (NSString *secret in @[@"nsec1fixturenotakey", @"  nostr:nsec1fixture", @"NSEC1FIXTURE", @"ncryptsec1fixture", @"npub1x nsec1fixture"]) {
        [editor insertText:secret replacementRange:editor.selectedRange];                 /* typing */
        [pb clearContents]; [pb setString:secret forType:NSPasteboardTypeString];
        Choose(key, @selector(paste:));                                                    /* Edit > Paste */
        [editor readSelectionFromPasteboard:pb type:NSPasteboardTypeString];               /* drag and drop */
        [editor readSelectionFromPasteboard:pb];                                           /* Services */
        Turn();
        CHECK(!HasSecret(editor.string) && !HasSecret(field.stringValue));
    }
    CHECK(!stored);
    if (message) CHECK(LabelContains(messageView, message));
    /* Undo and Redo both run, and neither restores the refused text. */
    CHECK(Choose(key, @selector(undo:)) && !HasSecret(editor.string) && ![editor.string isEqual:@"abc"]);
    Turn();
    CHECK(Choose(key, @selector(redo:)) && [editor.string isEqual:@"abc"]);
    Turn();
    /* The only undoable change is the legitimate "abc": one more Undo
     * empties the field and nothing older remains to restore. */
    CHECK(Choose(key, @selector(undo:)) && !editor.string.length && !HasSecret(editor.string) && !stored);
    Turn();
    CHECK(!Enabled(key, @selector(undo:)));
    CHECK(Choose(key, @selector(redo:)) && [editor.string isEqual:@"abc"] && !stored);
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
@end
@implementation FixtureBook
- (BOOL)canReview { return YES; }
- (BOOL)canEdit { return YES; }
- (BOOL)canReplaceReview:(MDHostReview *)review { return review.oldKey!=nil; }
- (BOOL)canConfirmReview:(MDHostReview *)review fingerprint:(NSString *)fingerprint {
    return [self canReplaceReview:review] && [review.fingerprint isEqual:fingerprint];
}
@end
@interface FixtureHosts : MDHostsWorker
@property NSUInteger resolves,saves,launches,closes;
@property BOOL mismatch,observeCancellation;
@property(strong) NSString *input,*confirmation;
@property(strong) MDHostReview *savedReview,*launchReview;
@property(copy) dispatch_block_t entered;
@property(strong) dispatch_semaphore_t proceed;
@end
@implementation FixtureHosts
- (BOOL)openDirectory:(NSString *)directory error:(NSString **)error { (void)directory; (void)error; return YES; }
- (MDHostsBook *)snapshot { return [FixtureBook new]; }
- (BOOL)startRelays:(NSString *)urls { (void)urls; return YES; }
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
    CHECK(Refuses(window, Field(view,@"Optional IPv4"), view, @"Private keys are not host addresses"));
    CHECK(Refuses(window, address, view, @"Private keys are not host addresses"));
    NSString *config=[@(getenv("XDG_CONFIG_HOME")) stringByAppendingPathComponent:@"metadesk"];
    CHECK([NSFileManager.defaultManager createDirectoryAtPath:config withIntermediateDirectories:YES
        attributes:@{NSFilePosixPermissions:@0700} error:NULL]);
    NSString *prefs=[config stringByAppendingPathComponent:@"manager-relays.ini"];
    NSString *corrupt=@"not a relay preferences file";
    CHECK([corrupt writeToFile:prefs atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    [page activate]; CHECK(Await(^BOOL{ return review.enabled; }));
    /* View > Refresh and the toolbar's Refresh reach the page as refresh:. */
    NSMenuItem *refresh=MenuItem([MDManagerApp makeMainMenu],@selector(refresh:));
    BOOL (^canRefresh)(void)=^BOOL{ return [page validateMenuItem:refresh]; };
    CHECK(canRefresh() && [NSApp sendAction:@selector(refresh:) to:page from:refresh] && Await(^BOOL{ return window.attachedSheet!=nil; }));
    CHECK(EndSheet(window,NSAlertFirstButtonReturn));
    CHECK([corrupt isEqual:[NSString stringWithContentsOfFile:prefs encoding:NSUTF8StringEncoding error:NULL]]);
    CHECK(canRefresh() && [NSApp sendAction:@selector(refresh:) to:page from:refresh] && Await(^BOOL{ return window.attachedSheet!=nil; }));
    CHECK(EndSheet(window,NSAlertSecondButtonReturn));
    CHECK(Await(^BOOL{ return canRefresh() && !LabelContains(view,@"Could not read saved bootstrap"); }));
    CHECK(![corrupt isEqual:[NSString stringWithContentsOfFile:prefs encoding:NSUTF8StringEncoding error:NULL]]);
    CHECK([@"[relays]\nbootstrap=wss://fixture.invalid/;\n" writeToFile:prefs atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    __block BOOL drained=NO;
    [page suspendForRoleEdit:^{ drained=YES; }];
    CHECK(Await(^BOOL{ return drained; }));
    [page resumeAfterRoleEdit];
    CHECK(Await(^BOOL{ return LabelContains(view,@"Relay sync is paused"); }));

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
@property BOOL closed;
@end
@implementation FixtureLink
- (void)command:(NSDictionary *)cmd policyVersion:(NSNumber *)version {
    MD_CHECK(cmd!=nil && !version); if (cmd) [self.commands addObject:cmd];
}
- (void)close:(dispatch_block_t)drained { self.closed=YES; drained(); }
- (void)connect:(NSString *)path { (void)path; }
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
/* B2: the Relays section's host identity field refuses secret keys too. */
static BOOL RelaysField(MDManagerApp *app, NSWindow *window) {
    [app showSection:@"Relays"];
    NSView *root=window.contentView;
    CHECK(Refuses(window, Field(root,@"Host identity"), root, @"That’s a private key"));
    return YES;
}
/* S9: a late pending-request update after the app delegate is gone is ignored. */
static BOOL LateBadge(void) {
    FixtureLink *link=[[FixtureLink alloc] initWithUpdate:nil]; link.commands=[NSMutableArray array];
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
static BOOL Run(NSString *directory) {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    FixtureHosts *worker=[[FixtureHosts alloc] initWithChange:nil];
    MDHostsPage *hosts=[[MDHostsPage alloc] initWithWorker:worker];
    FixtureLink *link=[[FixtureLink alloc] initWithUpdate:nil]; link.commands=[NSMutableArray array];
    MDLocalHostPages *access=[[MDLocalHostPages alloc] initWithLinkFactory:^MDHostLink *(MDHostUpdate update) {
        link.push=update; return link;
    }];
    FixtureIdentity *identity=[[FixtureIdentity alloc] initWithHosts:hosts directory:directory connect:Connect credentials:^{
        static const MdCredentialStoreOps ops={.get=GetPair,.put=PutPair,.remove=RemovePair};
        return md_credential_store_custom(&ops,NULL);
    }];
    /* The real main window and controller hierarchy, never ordered on screen. */
    FixtureApp *app=[[FixtureApp alloc] initWithHosts:hosts local:access identity:identity];
    NSWindow *window=[app makeMainWindow];
    window.alphaValue=0;
    KeyWindowOverride=window;
    NSApp.delegate=app; /* as in production: app-level commands resolve here */
    CHECK(Menus());
    CHECK(Persistence(app,window));
    [app showSection:@"Hosts"];
    CHECK(Hosts(hosts,worker,window));
    CHECK(Editing(app,window,hosts));
    CHECK(Commands(app,window,hosts));
    [app showSection:@"Access"];
    CHECK(Access(access,link,window));
    CHECK(RelaysField(app,window));
    MdSignerType backend=MDIdentitySupportsBackend(MD_SIGNER_NIP46) ? MD_SIGNER_NIP46 :
        MDIdentitySupportsBackend(MD_SIGNER_NIP5F) ? MD_SIGNER_NIP5F : MD_SIGNER_NIP55L;
    [app showSection:@"Identity"];
    if (MDIdentitySupportsBackend(backend)) CHECK(Identity(identity,directory,backend));
    else puts("SKIP: no compiled external signer adapter for the Identity action fixture");
    [app showSection:@"Hosts"];
    CHECK(Quit(app,hosts,worker,link,window));
    KeyWindowOverride=nil;
    NSApp.delegate=nil;
    [window close];
    CHECK(LateBadge());
    return YES;
}
int main(int argc,char **argv) {
    @autoreleasepool {
        MD_CHECK(argc==2);
        CFDictionaryRef session=CGSessionCopyCurrentDictionary();
        if (!session || CGMainDisplayID()==kCGNullDirectDisplay) {
            if (session) CFRelease(session);
            puts("SKIP: no WindowServer session for the AppKit controller fixture");
            return 77;
        }
        CFRelease(session);
        [TestApplication sharedApplication];
        if (argc==2) Run(@(argv[1]));
        return md_test_report();
    }
}
