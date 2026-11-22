/* Native AX adapter with owned CF objects in place of WindowServer IPC. */
#import <ApplicationServices/ApplicationServices.h>
#include "md_test.h"
#include <string.h>
static AXUIElementRef front;
static AXError hit_error, press_error;
static int target_presses, occluder_presses;
static CFStringRef target, occluder;
static AXUIElementRef fake_desktop(void) { return (AXUIElementRef)CFRetain(CFSTR("desktop")); }
static Boolean fake_trusted(void) { return true; }
static AXError fake_pid(AXUIElementRef elem, pid_t *pid) { (void)elem; *pid = 123; return kAXErrorSuccess; }
static AXError fake_hit(AXUIElementRef desktop, float x, float y, AXUIElementRef *out) {
    (void)desktop;
    MD_CHECK(x == 140 && y == 220);
    *out = front ? (AXUIElementRef)CFRetain(front) : NULL;
    return hit_error;
}
static AXError fake_press(AXUIElementRef elem, CFStringRef action) {
    MD_CHECK(CFEqual(action, kAXPressAction));
    if (press_error) return press_error;
    if (CFEqual(elem, target)) target_presses++; else occluder_presses++;
    return kAXErrorSuccess;
}
#define AXUIElementCreateSystemWide fake_desktop
#define AXIsProcessTrusted fake_trusted
#define AXUIElementGetPid fake_pid
#define AXUIElementCopyElementAtPosition fake_hit
#define AXUIElementPerformAction fake_press
#include "../src/core/a11y_axui.m"
int main(void) {
    target = CFSTR("owned-target"); occluder = CFSTR("owned-occluder");
    AXUIState state = { .connected = 1 };
    MdA11yCtx ctx = { .vtable = &axui_backend, .backend_data = &state };
    char *id = axui_element_id((AXUIElementRef)target);
    front = (AXUIElementRef)occluder;
    MD_CHECK(!md_a11y_target_at_point(&ctx, id, 140, 220));
    MD_CHECK(axui_click_target(&ctx, id, 140, 220) == -1);
    MD_CHECK(target_presses == 0 && occluder_presses == 0);
    front = (AXUIElementRef)target;
    MD_CHECK(md_a11y_target_at_point(&ctx, id, 140, 220));
    MD_CHECK(axui_click_target(&ctx, id, 140, 220) == 0);
    MD_CHECK(target_presses == 1 && occluder_presses == 0);
    press_error = kAXErrorActionUnsupported;
    MD_CHECK(axui_click_target(&ctx, id, 140, 220) == -1);
    hit_error = kAXErrorCannotComplete;
    MD_CHECK(!md_a11y_target_at_point(&ctx, id, 140, 220));
    MD_CHECK(axui_click_target(&ctx, id, 140, 220) == -1);
    hit_error = kAXErrorSuccess; front = NULL;
    MD_CHECK(!md_a11y_target_at_point(&ctx, id, 140, 220));
    free(id);
    return md_test_report();
}
