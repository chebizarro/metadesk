/*
 * metadesk — render.h
 * SDL2 frame display for human client.
 *
 * Creates an SDL2 window with a streaming texture. Decoded RGBA frames
 * are uploaded to the texture and presented. The window auto-resizes
 * when the stream resolution changes.
 *
 * vsync is disabled in Phase 1 per spec §9.1 (<2ms present target).
 */
#ifndef MD_RENDER_H
#define MD_RENDER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque renderer context */
typedef struct MdRenderer MdRenderer;

/* Input event types for MdInputCallback */
typedef enum {
    MD_INPUT_KEY,            /* a=scancode, b=pressed (1=down, 0=up)   */
    MD_INPUT_MOUSE_MOVE,     /* a=x, b=y (window coordinates)          */
    MD_INPUT_MOUSE_BUTTON,   /* a=button (SDL button id), b=pressed    */
    MD_INPUT_SCROLL,         /* a=scroll_x, b=scroll_y                 */
} MdInputType;

/* Callback invoked for keyboard and mouse events.
 * type: one of MdInputType.
 * a, b: type-specific parameters (see enum comments).
 * userdata: opaque pointer passed to md_renderer_set_input_callback. */
typedef void (*MdInputCallback)(int type, int a, int b, void *userdata);

/* Create renderer with initial window size.
 * title: window title string.
 * Returns NULL on failure (SDL2 not available, etc.). */
MdRenderer *md_renderer_create(uint32_t width, uint32_t height, const char *title);

/* Present a decoded RGBA frame.
 * If width/height differ from the current texture, the texture is
 * recreated and the window is resized to match.
 * Returns 0 on success, -1 on error. */
int md_renderer_present(MdRenderer *r, const uint8_t *rgba,
                        uint32_t width, uint32_t height);

/* Process window events (close, resize, keyboard, mouse).
 * Returns 0 if should continue, -1 if quit requested (window closed,
 * Escape pressed, or SDL_QUIT event). */
int md_renderer_poll_events(MdRenderer *r);

/* Get the current window dimensions (may differ from stream resolution). */
int md_renderer_get_window_size(const MdRenderer *r, uint32_t *w, uint32_t *h);

/* Get the SDL window and renderer handles (for ImGui integration).
 * Returns opaque pointers — caller casts to SDL_Window* / SDL_Renderer*.
 * Returns NULL if renderer is not initialized. */
void *md_renderer_get_sdl_window(MdRenderer *r);
void *md_renderer_get_sdl_renderer(MdRenderer *r);

/* Check if window is still open. */
bool md_renderer_is_open(const MdRenderer *r);

/* Register a callback for keyboard/mouse input events.
 * The callback fires during md_renderer_poll_events() for every
 * key, mouse move, mouse button, and scroll event. */
void md_renderer_set_input_callback(MdRenderer *r, MdInputCallback cb, void *userdata);

/* Set the host screen dimensions for coordinate scaling.
 * Mouse coordinates are scaled from client window to host screen. */
void md_renderer_set_host_size(MdRenderer *r, uint32_t host_w, uint32_t host_h);

/* Destroy renderer, close window, free SDL resources. */
void md_renderer_destroy(MdRenderer *r);

#ifdef __cplusplus
}
#endif

#endif /* MD_RENDER_H */
