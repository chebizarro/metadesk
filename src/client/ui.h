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
#include "quality.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque overlay context */
typedef struct MdOverlay MdOverlay;

typedef enum {
    MD_CLIENT_IDLE, MD_CLIENT_FIPS, MD_CLIENT_NEGOTIATING,
    MD_CLIENT_CONNECTING, MD_CLIENT_BINDING, MD_CLIENT_STREAMING,
    MD_CLIENT_BACKOFF,
} MdClientPhase;
typedef enum {
    MD_CLIENT_ERROR_NONE, MD_CLIENT_ERROR_FIPS, MD_CLIENT_ERROR_ADMISSION,
    MD_CLIENT_ERROR_TRANSPORT, MD_CLIENT_ERROR_BINDING,
} MdClientError;
typedef enum {
    MD_OVERLAY_NONE, MD_OVERLAY_DISCONNECT, MD_OVERLAY_RETRY, MD_OVERLAY_QUIT,
} MdOverlayAction;

const char *md_client_phase_label(MdClientPhase phase);
const char *md_client_error_label(MdClientError error);
MdOverlayAction md_overlay_take_action(MdOverlay *o);

/* Overlay stats shown to user */
typedef struct {
    MdQuality quality;
    bool quality_supported;
    float  latency_ms;       /* total pipeline latency             */
    float  encode_ms;        /* average encode time                */
    float  decode_ms;        /* average decode time                */
    float  rtt_ms;           /* network round-trip time            */
    MdClientPhase phase;
    MdClientError error;
    uint32_t granted_caps;
    bool   paused;
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
 * SDL events arrive via md_overlay_handle_sdl_event() from the
 * renderer's single event pump. */
void md_overlay_new_frame(MdOverlay *o);

/* Handle one SDL event (fed by the renderer's event pump).
 * `event` is an SDL_Event*. Handles ImGui input + the F1 toggle. */
/* Returns true for reserved F1/Escape keys (including key releases). */
bool md_overlay_handle_sdl_event(MdOverlay *o, void *event);

/* Render the overlay with current stats.
 * Call between md_overlay_new_frame() and the SDL_RenderPresent(). */
void md_overlay_render(MdOverlay *o, const MdOverlayStats *stats);

/* True if ImGui is currently consuming mouse / keyboard input (e.g. the
 * pointer is over the overlay panel or a widget is focused). The client
 * uses these to avoid forwarding those events to the remote host. */
bool md_overlay_wants_mouse(MdOverlay *o);
bool md_overlay_wants_keyboard(MdOverlay *o);

bool md_overlay_take_quality(MdOverlay *o, MdQuality *quality);

/* Destroy overlay and ImGui context. */
void md_overlay_destroy(MdOverlay *o);

#ifdef __cplusplus
}
#endif

#endif /* MD_UI_H */
