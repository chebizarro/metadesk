/* Owned AppKit target: animated frame counter plus an accessible action button.
 * clang tools/e2e/macos-fixture.m -fobjc-arc -framework Cocoa -o build/macos-fixture
 * No global hooks/input injection. All interaction stays in this process's window. */
#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

@interface OwnedButton : NSButton
@end
@implementation OwnedButton
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
@end

@interface Fixture : NSObject <NSApplicationDelegate>
@property NSWindow *window;
@property NSTextField *frames;
@property NSTextField *clicks;
@property NSUInteger frameCount;
@property NSUInteger clickCount;
@property NSWindow *occluder;
@property NSTextField *occluderClicks;
@property NSUInteger occluderClickCount;
@property BOOL testOcclusion;
@end
@implementation Fixture
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 640, 400)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"md-l7 owned macOS target";
    self.window.releasedWhenClosed = NO;
    self.frames = [NSTextField labelWithString:@"md-l7 frame 0"];
    self.frames.frame = NSMakeRect(30, 310, 550, 40);
    self.frames.font = [NSFont monospacedSystemFontOfSize:26 weight:NSFontWeightRegular];
    self.clicks = [NSTextField labelWithString:@"md-l7 clicks 0"];
    self.clicks.frame = NSMakeRect(30, 250, 550, 40);
    NSButton *button = [OwnedButton buttonWithTitle:@"md-l7 action" target:self action:@selector(clicked:)];
    button.frame = NSMakeRect(30, 180, 220, 45);
    button.accessibilityIdentifier = @"md-l7-action";
    NSTextField *entry = [[NSTextField alloc] initWithFrame:NSMakeRect(30, 100, 550, 40)];
    entry.placeholderString = @"md-l7 owned text input";
    entry.accessibilityLabel = @"md-l7 owned text input";
    [self.window.contentView addSubview:self.frames];
    [self.window.contentView addSubview:self.clicks];
    [self.window.contentView addSubview:button];
    [self.window.contentView addSubview:entry];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (self.testOcclusion) {
        // Deliberately occlude the target with another OWNED window. Any
        // coordinate-based mis-targeting lands in this fixture, not another app.
        self.window.level = NSFloatingWindowLevel;
        self.occluder = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 640, 400)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        self.occluder.title = @"md-l7 owned occluder";
        self.occluder.level = NSFloatingWindowLevel;
        self.occluder.releasedWhenClosed = NO;
        OwnedButton *cover = [OwnedButton buttonWithTitle:@"md-l7 occluder action"
            target:self action:@selector(occluderClicked:)];
        cover.frame = button.frame;
        self.occluderClicks = [NSTextField labelWithString:@"md-l7 occluder clicks 0"];
        self.occluderClicks.frame = self.clicks.frame;
        [self.occluder.contentView addSubview:cover];
        [self.occluder.contentView addSubview:self.occluderClicks];
        [self.occluder makeKeyAndOrderFront:nil];
    }
    __weak Fixture *weakSelf = self;
    [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        Fixture *s = weakSelf;
        s.frames.stringValue = [NSString stringWithFormat:@"md-l7 frame %lu", (unsigned long)++s.frameCount];
    }];
    printf("fixture ready pid=%d\n", getpid()); fflush(stdout);
}
- (void)clicked:(id)sender {
    (void)sender;
    self.clicks.stringValue = [NSString stringWithFormat:@"md-l7 clicks %lu", (unsigned long)++self.clickCount];
    printf("owned button clicked count=%lu\n", (unsigned long)self.clickCount); fflush(stdout);
}
- (void)occluderClicked:(id)sender {
    (void)sender;
    self.occluderClicks.stringValue = [NSString stringWithFormat:@"md-l7 occluder clicks %lu",
        (unsigned long)++self.occluderClickCount];
    printf("OWNED OCCLUDER clicked count=%lu target-clicks=%lu\n",
        (unsigned long)self.occluderClickCount, (unsigned long)self.clickCount); fflush(stdout);
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app; return YES;
}
@end
int main(int argc, const char **argv) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        Fixture *fixture = [Fixture new];
        fixture.testOcclusion = argc == 2 && strcmp(argv[1], "--occlusion-test") == 0;
        app.delegate = fixture;
        [app run];
    }
    return 0;
}
