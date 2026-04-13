/*
 * metadesk — input_cgevent.m
 * macOS input backend: CGEvent / Quartz Event Services.
 *
 * TODO: Implement (see spec §2.3.3 and bead metadesk-gjh).
 */
#include "input.h"

#include <stdio.h>

/* Stub — returns NULL until implemented */
const MdInputBackend *md_input_backend_create(void) {
    fprintf(stderr, "input: CGEvent backend not yet implemented\n");
    return NULL;
}
