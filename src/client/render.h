/*
 * metadesk — render.h
 * SDL2 frame display for human client.
 */
#ifndef MD_RENDER_H
#define MD_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque renderer context */
typedef struct MdRenderer MdRenderer;

/* Create renderer with window of given size. */
MdRenderer *md_renderer_create(uint32_t width, uint32_t height, const char *title);

/* Present a decoded RGBA frame. */
int md_renderer_present(MdRenderer *r, const uint8_t *rgba,
                        uint32_t width, uint32_t height);

/* Process window events. Returns 0 if should continue, -1 if quit requested. */
int md_renderer_poll_events(MdRenderer *r);

/* Destroy renderer. */
void md_renderer_destroy(MdRenderer *r);

#ifdef __cplusplus
}
#endif

#endif /* MD_RENDER_H */
