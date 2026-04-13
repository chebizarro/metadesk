/*
 * metadesk — capture_screencapturekit.m
 * macOS capture backend: ScreenCaptureKit.
 *
 * TODO: Implement (see spec §2.3.1 and bead metadesk-0gi).
 * Requires macOS 13.0+.
 */
#include "capture.h"

#include <stdio.h>

/* Stub — returns NULL until implemented */
const MdCaptureBackend *md_capture_backend_create(void) {
    fprintf(stderr, "capture: ScreenCaptureKit backend not yet implemented\n");
    return NULL;
}
