/*
 * metadesk — ui.h
 * Dear ImGui overlay for the human client.
 *
 * The overlay renders on top of the video frame texture
 * using the SDL2 renderer backend. It shows:
 *   - Connection status
 *   - Latency (encode + decode + RTT)
 *   - FPS and bitrate
 *   - Disconnect button
 *
 * Requires an active SDL2 window and renderer from render.c.
 * ImGui context is created and destroyed with the overlay.
 */
#ifndef MD_UI_H
#define MD_UI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque overlay context */
typedef struct MdOverlay MdOverlay;

/* Overlay stats shown to user */
typedef struct {
    float  latency_ms;       /* total pipeline latency             */
    float  encode_ms;        /* average encode time                */
    float  decode_ms;        /* average decode time                */
    float  rtt_ms;           /* network round-trip time            */
    bool   connected;        /* true if stream is active           */
    bool   reconnecting;     /* true while client is retrying      */
    uint32_t reconnect_delay_ms; /* next retry delay, if reconnecting */
    const char *status_message;  /* optional connection detail        */
    int    fps;              /* current display FPS                */
    float  bitrate_mbps;     /* current bitrate in Mbps            */
    const char *encoder_name; /* "NVENC" or "x264"                */
} MdOverlayStats;

/* Create ImGui overlay attached to an SDL window/renderer.
 * sdl_window: SDL_Window* from md_renderer_get_sdl_window()
 * sdl_renderer: SDL_Renderer* from md_renderer_get_sdl_renderer()
 * Returns NULL if ImGui initialization fails. */
MdOverlay *md_overlay_create(void *sdl_window, void *sdl_renderer);

/* Begin a new overlay frame. Call before md_overlay_render().
 * Processes ImGui's share of SDL events internally. */
void md_overlay_new_frame(MdOverlay *o);

/* Render the overlay with current stats.
 * Call between md_overlay_new_frame() and the SDL_RenderPresent(). */
void md_overlay_render(MdOverlay *o, const MdOverlayStats *stats);

/* Destroy overlay and ImGui context. */
void md_overlay_destroy(MdOverlay *o);

#ifdef __cplusplus
}
#endif

#endif /* MD_UI_H */
