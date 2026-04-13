/*
 * metadesk — ui.h
 * Dear ImGui overlay for the human client.
 */
#ifndef MD_UI_H
#define MD_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque overlay context */
typedef struct MdOverlay MdOverlay;

/* Overlay stats shown to user */
typedef struct {
    float  latency_ms;
    int    connected;
    int    fps;
    float  bitrate_mbps;
} MdOverlayStats;

/* Create ImGui overlay (requires active SDL renderer). */
MdOverlay *md_overlay_create(void *sdl_window, void *sdl_renderer);

/* Render one frame of the overlay with current stats. */
void md_overlay_render(MdOverlay *o, const MdOverlayStats *stats);

/* Destroy overlay. */
void md_overlay_destroy(MdOverlay *o);

#ifdef __cplusplus
}
#endif

#endif /* MD_UI_H */
