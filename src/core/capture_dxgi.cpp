/*
 * metadesk — capture_dxgi.cpp
 * Windows capture backend: DXGI Desktop Duplication.
 *
 * TODO: Implement (see spec §2.3.1 and bead metadesk-df8).
 * Requires Windows 8+.
 */
extern "C" {
#include "capture.h"
}

#include <cstdio>

/* Stub — returns NULL until implemented */
extern "C"
const MdCaptureBackend *md_capture_backend_create(void) {
    fprintf(stderr, "capture: DXGI backend not yet implemented\n");
    return nullptr;
}
