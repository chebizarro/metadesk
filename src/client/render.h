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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque renderer context */
typedef struct MdRenderer MdRenderer;

/* Create renderer with initial window size.
 * title: window title string.
 * Returns NULL on failure (SDL2 not available, etc.). */
MdRenderer *md_renderer_create(uint32_t width, uint32_t height, const char *title);

/* Initialize SDL video and require a driver with visible output. Headless
 * drivers (offscreen, dummy, evdev) are refused rather than used silently.
 * Logs the selected driver to stderr. Idempotent; md_renderer_create() calls
 * it. Returns 0 on success, -1 (reason on stderr) if no display is usable. */
int md_renderer_open_display(void);

/* Shut SDL down after every renderer is destroyed. */
void md_renderer_close_display(void);

/* Renderer selection (md-rnc). One SDL render driver, in SDL's list order. */
typedef struct {
    const char *name;
    bool        accelerated;   /* SDL_RENDERER_ACCELERATED */
} MdRenderDriver;

/* False if creating the named driver cannot work here; such a driver is left
 * out of the plan rather than tried (SDL's GL drivers crash, rather than
 * fail, when their libraries are missing). */
typedef bool (*MdRenderDriverUsable)(const char *name, void *userdata);

/* Try order for SDL render drivers: the driver named by `hint`
 * (SDL_RENDER_DRIVER, case-insensitive) first if it exists, then the other
 * accelerated drivers in SDL's order (opengl, opengles2, opengles on Linux),
 * then the rest (software). Drivers `usable` rejects are omitted; NULL
 * `usable` accepts all. Writes at most `cap` indices into `drivers` to
 * `order` and returns how many. */
size_t md_render_driver_plan(const MdRenderDriver *drivers, size_t n,
                             const char *hint, MdRenderDriverUsable usable,
                             void *userdata, int *order, size_t cap);

/* Value for SDL_HINT_FRAMEBUFFER_ACCELERATION once the plan has skipped a
 * driver. SDL's software renderer presents through a window framebuffer, and
 * where the video driver has none (Wayland) SDL builds one on the first
 * non-software driver it can create, ignoring the plan. Naming the first
 * planned accelerated driver (or "0", no texture framebuffer, if none is
 * planned) keeps it off the skipped one. */
const char *md_render_framebuffer_accel(const MdRenderDriver *drivers,
                                        const int *order, size_t planned);

/* Present a decoded RGBA frame in one call (upload + draw + present).
 * Kept for simple callers; the client composite path uses the split
 * update/begin/end functions below so the overlay shares the same frame. */
int md_renderer_present(MdRenderer *r, const uint8_t *rgba,
                        uint32_t width, uint32_t height);

/* Upload a decoded RGBA frame to the streaming texture without drawing or
 * presenting. The texture is (re)created and the window resized on a
 * resolution change. Returns 0 on success, -1 on error. */
int md_renderer_update_frame(MdRenderer *r, const uint8_t *rgba,
                             uint32_t width, uint32_t height);

/* Begin a composite frame: reset scale to 1:1, clear, and draw the latest
 * video texture to fill the window. Draw the overlay after this, then call
 * md_renderer_end_frame() to present — exactly one present per displayed
 * frame, so the overlay never flip-books against the video. */
void md_renderer_begin_frame(MdRenderer *r);

/* Finish a composite frame: present the back buffer. */
void md_renderer_end_frame(MdRenderer *r);

/* Process window events (close, resize).
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

/* Register a callback invoked for every SDL event during
 * md_renderer_poll_events(), before the renderer's own handling.
 * `event` is an SDL_Event* (opaque here to keep SDL out of the header).
 * Used by the ImGui overlay so a single event pump serves both. */
typedef void (*MdSdlEventCallback)(void *event, void *userdata);
void md_renderer_set_event_callback(MdRenderer *r, MdSdlEventCallback cb,
                                    void *userdata);

/* Recompute HiDPI scale factors (output pixels vs window points).
 * Called automatically on create and window resize; may also be called
 * manually after display changes. */
void md_renderer_update_hidpi_scale(MdRenderer *r);

/* Destroy renderer, close window, free SDL resources. */
void md_renderer_destroy(MdRenderer *r);

#ifdef __cplusplus
}
#endif

#endif /* MD_RENDER_H */
