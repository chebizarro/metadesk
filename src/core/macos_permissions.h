#ifndef MD_MACOS_PERMISSIONS_H
#define MD_MACOS_PERMISSIONS_H

#include <stdbool.h>
#include <ApplicationServices/ApplicationServices.h>

/* These report only the calling process's authorization. Never substitute
 * permission requests, capture enumeration, or test events for these probes. */
static inline bool md_macos_screen_recording_allowed(void) {
    return CGPreflightScreenCaptureAccess();
}

static inline bool md_macos_accessibility_allowed(void) {
    return AXIsProcessTrusted();
}

#endif
