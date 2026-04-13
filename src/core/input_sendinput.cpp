/*
 * metadesk — input_sendinput.cpp
 * Windows input backend: SendInput API.
 *
 * TODO: Implement (see spec §2.3.3 and bead metadesk-qbg).
 * Requires Windows XP+.
 */
extern "C" {
#include "input.h"
}

#include <cstdio>

/* Stub — returns NULL until implemented */
extern "C"
const MdInputBackend *md_input_backend_create(void) {
    fprintf(stderr, "input: SendInput backend not yet implemented\n");
    return nullptr;
}
