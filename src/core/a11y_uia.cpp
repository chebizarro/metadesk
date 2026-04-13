/*
 * metadesk — a11y_uia.cpp
 * Windows accessibility backend: UI Automation.
 *
 * TODO: Implement (see spec §2.3.2 and bead metadesk-qzu).
 * Requires Windows 7+.
 */
extern "C" {
#include "a11y.h"
}

#include <cstdio>

/* Stub — returns NULL until implemented */
extern "C"
const MdA11yBackend *md_a11y_backend_create(void) {
    fprintf(stderr, "a11y: UI Automation backend not yet implemented\n");
    return nullptr;
}
