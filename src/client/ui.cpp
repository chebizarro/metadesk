/*
 * metadesk — ui.cpp
 * Dear ImGui overlay for the human client.
 *
 * Renders a translucent stats panel on top of the video frame using
 * ImGui with the SDL2 + SDL_Renderer backends. The overlay shows:
 *   - Connection status (connected/disconnected)
 *   - Latency breakdown (encode, decode, RTT, total)
 *   - FPS counter and bitrate
 *   - Encoder name (NVENC/x264)
 *   - Disconnect button
 *
 * Toggle visibility with F1 or md_overlay_toggle().
 *
 * The overlay consumes minimal CPU: ImGui only draws when visible,
 * and the stats window uses fixed-size layout to avoid per-frame
 * text measurement.
 */
#include "ui.h"

#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>

#include <SDL2/SDL.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>

/* ── Overlay state ───────────────────────────────────────────── */

struct MdOverlay {
    bool           visible;
    bool           wants_input;
    bool           disconnect_requested;
    SDL_Window    *window;
    SDL_Renderer  *renderer;

    /* Smoothed stats for display (avoid jitter) */
    float          smooth_latency;
    float          smooth_encode;
    float          smooth_decode;
    float          smooth_rtt;
    float          smooth_fps;
    float          smooth_bitrate;
};

/* Exponential moving average smoothing factor */
static constexpr float SMOOTH_ALPHA = 0.1f;

static float smooth(float prev, float curr) {
    if (prev <= 0.0f) return curr;
    return prev * (1.0f - SMOOTH_ALPHA) + curr * SMOOTH_ALPHA;
}

/* ── Public API ──────────────────────────────────────────────── */

extern "C" {

MdOverlay *md_overlay_create(void *sdl_window, void *sdl_renderer) {
    if (!sdl_window || !sdl_renderer)
        return nullptr;

    auto *o = static_cast<MdOverlay *>(std::calloc(1, sizeof(MdOverlay)));
    if (!o) return nullptr;

    o->window   = static_cast<SDL_Window *>(sdl_window);
    o->renderer = static_cast<SDL_Renderer *>(sdl_renderer);
    o->visible  = true;

    /* Create ImGui context */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    /* Dark theme with transparency */
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.Alpha             = 0.85f;
    style.WindowBorderSize  = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.10f, 0.85f);
    style.Colors[ImGuiCol_Border]   = ImVec4(0.3f, 0.3f, 0.5f, 0.5f);

    /* Initialize backends */
    ImGui_ImplSDL2_InitForSDLRenderer(o->window, o->renderer);
    ImGui_ImplSDLRenderer2_Init(o->renderer);

    fprintf(stderr, "overlay: Dear ImGui %s initialized\n", IMGUI_VERSION);
    return o;
}

void md_overlay_new_frame(MdOverlay *o) {
    if (!o) return;

    /* Process SDL events for ImGui */
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT,
                          SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {
        ImGui_ImplSDL2_ProcessEvent(&event);

        /* Check for F1 toggle */
        if (event.type == SDL_KEYDOWN &&
            event.key.keysym.sym == SDLK_F1 &&
            !event.key.repeat) {
            o->visible = !o->visible;
        }
    }

    /* Start new ImGui frame */
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    /* Update input capture state */
    ImGuiIO &io = ImGui::GetIO();
    o->wants_input = io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void md_overlay_render(MdOverlay *o, const MdOverlayStats *stats) {
    if (!o || !stats) {
        /* Still need to end the frame even if not rendering stats */
        if (o) {
            ImGui::EndFrame();
        }
        return;
    }

    if (!o->visible) {
        /* Render a minimal "F1 for stats" hint in the corner */
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowBgAlpha(0.3f);
        ImGui::Begin("##hint", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.7f, 0.6f), "F1: stats");
        ImGui::End();

        /* Finalize and render */
        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(),
                                               o->renderer);
        return;
    }

    /* Smooth stats to avoid jitter */
    o->smooth_latency = smooth(o->smooth_latency, stats->latency_ms);
    o->smooth_encode  = smooth(o->smooth_encode,  stats->encode_ms);
    o->smooth_decode  = smooth(o->smooth_decode,  stats->decode_ms);
    o->smooth_rtt     = smooth(o->smooth_rtt,     stats->rtt_ms);
    o->smooth_fps     = smooth(o->smooth_fps,     (float)stats->fps);
    o->smooth_bitrate = smooth(o->smooth_bitrate, stats->bitrate_mbps);

    /* Position in top-left corner */
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_FirstUseEver);

    ImGui::Begin("metadesk", nullptr,
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_AlwaysAutoResize);

    /* ── Connection status ───────────────────────────────── */
    if (stats->connected) {
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f),
                           "\xE2\x97\x89 Connected");  /* ◉ */
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f),
                           "\xE2\x97\x8B Disconnected");  /* ○ */
    }

    if (stats->encoder_name) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.8f, 1.0f),
                           "(%s)", stats->encoder_name);
    }

    ImGui::Separator();

    /* ── Latency breakdown ───────────────────────────────── */
    ImGui::Text("Latency");
    ImGui::Indent(10.0f);

    /* Color-code total latency */
    ImVec4 lat_color;
    if (o->smooth_latency < 16.0f)
        lat_color = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);    /* green: <16ms */
    else if (o->smooth_latency < 33.0f)
        lat_color = ImVec4(0.9f, 0.9f, 0.2f, 1.0f);    /* yellow: <33ms */
    else
        lat_color = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);    /* red: >33ms */

    ImGui::TextColored(lat_color, "Total:  %5.1f ms", o->smooth_latency);
    ImGui::Text("Encode: %5.1f ms", o->smooth_encode);
    ImGui::Text("Decode: %5.1f ms", o->smooth_decode);
    ImGui::Text("RTT:    %5.1f ms", o->smooth_rtt);

    ImGui::Unindent(10.0f);

    /* ── FPS & Bitrate ───────────────────────────────────── */
    ImGui::Separator();
    ImGui::Text("FPS: %.0f", o->smooth_fps);
    ImGui::SameLine(140);
    ImGui::Text("%.1f Mbps", o->smooth_bitrate);

    /* ── Disconnect button ───────────────────────────────── */
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.5f, 0.05f, 0.05f, 1.0f));

    float button_width = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Disconnect", ImVec2(button_width, 0))) {
        o->disconnect_requested = true;
        /* Post a quit event so the main loop exits cleanly */
        SDL_Event quit_event;
        quit_event.type = SDL_QUIT;
        SDL_PushEvent(&quit_event);
    }

    ImGui::PopStyleColor(3);

    /* ── Footer hint ─────────────────────────────────────── */
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 0.7f),
                       "F1: toggle overlay");

    ImGui::End();

    /* Finalize and render */
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(),
                                           o->renderer);
}

bool md_overlay_wants_input(const MdOverlay *o) {
    if (!o) return false;
    return o->wants_input;
}

void md_overlay_toggle(MdOverlay *o) {
    if (o) o->visible = !o->visible;
}

void md_overlay_destroy(MdOverlay *o) {
    if (!o) return;

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    std::free(o);
}

} /* extern "C" */
