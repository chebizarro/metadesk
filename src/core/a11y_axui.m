/*
 * metadesk — a11y_axui.m
 * macOS accessibility backend: AXUIElement.
 *
 * TODO: Implement (see spec §2.3.2 and bead metadesk-uq9).
 */
#include "a11y.h"

#include <stdio.h>

/* Stub — returns NULL until implemented */
const MdA11yBackend *md_a11y_backend_create(void) {
    fprintf(stderr, "a11y: AXUIElement backend not yet implemented\n");
    return NULL;
}
