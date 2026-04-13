/*
 * metadesk — ui.cpp
 * Dear ImGui overlay. Stub — implementation in M1.8.
 */
#include "ui.h"
#include <cstdlib>

struct MdOverlay {
    bool visible;
};

extern "C" {

MdOverlay *md_overlay_create(void *sdl_window, void *sdl_renderer) {
    (void)sdl_window; (void)sdl_renderer;
    MdOverlay *o = static_cast<MdOverlay*>(std::calloc(1, sizeof(MdOverlay)));
    if (o) o->visible = true;
    /* TODO: ImGui::CreateContext, ImGui_ImplSDL2_Init (M1.8) */
    return o;
}

void md_overlay_render(MdOverlay *o, const MdOverlayStats *stats) {
    if (!o || !stats) return;
    /* TODO: ImGui::NewFrame, render stats window (M1.8) */
}

void md_overlay_destroy(MdOverlay *o) {
    /* TODO: ImGui::DestroyContext */
    std::free(o);
}

} /* extern "C" */
