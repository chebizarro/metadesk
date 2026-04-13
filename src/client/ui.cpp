/*
 * metadesk — ui.cpp
 * Dear ImGui overlay. Stub — full implementation in M1.8.
 *
 * This stub provides the API surface so the client can compile
 * and link without ImGui. The M1.8 milestone replaces this with
 * a full ImGui_ImplSDL2 + ImGui_ImplSDLRenderer2 implementation.
 */
#include "ui.h"
#include <cstdlib>

struct MdOverlay {
    bool visible;
    bool wants_input;
};

extern "C" {

MdOverlay *md_overlay_create(void *sdl_window, void *sdl_renderer) {
    (void)sdl_window; (void)sdl_renderer;
    auto *o = static_cast<MdOverlay*>(std::calloc(1, sizeof(MdOverlay)));
    if (o) o->visible = true;
    /* TODO (M1.8): ImGui::CreateContext, ImGui_ImplSDL2_Init,
     * ImGui_ImplSDLRenderer2_Init */
    return o;
}

void md_overlay_new_frame(MdOverlay *o) {
    if (!o) return;
    /* TODO (M1.8): ImGui_ImplSDLRenderer2_NewFrame,
     * ImGui_ImplSDL2_NewFrame, ImGui::NewFrame */
}

void md_overlay_render(MdOverlay *o, const MdOverlayStats *stats) {
    if (!o || !o->visible || !stats) return;
    /* TODO (M1.8): render stats window via ImGui:
     *   - Connection status
     *   - Latency breakdown (encode/decode/RTT)
     *   - FPS counter
     *   - Bitrate
     *   - Disconnect button
     * ImGui::Render(), ImGui_ImplSDLRenderer2_RenderDrawData() */
}

bool md_overlay_wants_input(const MdOverlay *o) {
    if (!o) return false;
    return o->wants_input;
}

void md_overlay_toggle(MdOverlay *o) {
    if (o) o->visible = !o->visible;
}

void md_overlay_destroy(MdOverlay *o) {
    /* TODO (M1.8): ImGui_ImplSDLRenderer2_Shutdown,
     * ImGui_ImplSDL2_Shutdown, ImGui::DestroyContext */
    std::free(o);
}

} /* extern "C" */
