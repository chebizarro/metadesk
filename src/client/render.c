/*
 * metadesk — render.c
 * SDL2 frame rendering. Stub — implementation in M1.2/M1.8.
 */
#include "render.h"
#include <stdlib.h>

struct MdRenderer {
    uint32_t width;
    uint32_t height;
};

MdRenderer *md_renderer_create(uint32_t width, uint32_t height, const char *title) {
    (void)title;
    MdRenderer *r = calloc(1, sizeof(MdRenderer));
    if (r) {
        r->width = width;
        r->height = height;
    }
    /* TODO: SDL_CreateWindow + SDL_CreateRenderer (M1.2) */
    return r;
}

int md_renderer_present(MdRenderer *r, const uint8_t *rgba,
                        uint32_t width, uint32_t height) {
    if (!r || !rgba) return -1;
    (void)width; (void)height;
    /* TODO: update SDL texture and present (M1.2) */
    return 0;
}

int md_renderer_poll_events(MdRenderer *r) {
    if (!r) return -1;
    /* TODO: SDL_PollEvent (M1.2) */
    return 0;
}

void md_renderer_destroy(MdRenderer *r) {
    /* TODO: SDL cleanup */
    free(r);
}
