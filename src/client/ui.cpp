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
 * Toggle visibility with F1.
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
#include "session.h"
#include <jansson.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>

/* ── Overlay state ───────────────────────────────────────────── */

struct MdOverlay {
    bool           visible;
    MdOverlayAction action;
    bool quick_open;
    char quick_pin[65];
    char output[32];
    bool output_changed;
    MdQuality quality;
    bool quality_changed;
    int quality_width, quality_height, quality_bitrate, quality_fps;
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

/* Draw the finished ImGui frame.
 *
 * The renderer is already at the HiDPI scale (set by md_renderer_begin_frame),
 * so the SDLRenderer2 backend passes vertex positions in logical points and
 * SDL scales them to pixels — the same coordinate space ImGui uses for layout
 * and mouse hit-testing. Do NOT change the scale here: resetting it would put
 * the overlay's drawn geometry and its hit-testing in different spaces and
 * break dragging.
 *
 * No present here either: the caller draws the video first
 * (md_renderer_begin_frame), then the overlay, then presents once
 * (md_renderer_end_frame) — one present per frame, no flip-book trails. */
static void render_draw_data(MdOverlay *o) {
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
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

    /* Events arrive via md_overlay_handle_sdl_event() from the
     * renderer's single pump — no second consumer of the SDL queue. */
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

bool md_overlay_handle_sdl_event(MdOverlay *o, void *event) {
    if (!o || !event) return false;

    SDL_Event *ev = static_cast<SDL_Event *>(event);
    ImGui_ImplSDL2_ProcessEvent(ev);

    /* F1 toggles overlay visibility */
    if (ev->type == SDL_KEYDOWN &&
        ev->key.keysym.sym == SDLK_F1 &&
        !ev->key.repeat) {
        o->visible = !o->visible;
    }
    if (ev->type == SDL_KEYDOWN && ev->key.keysym.sym == SDLK_ESCAPE && !ev->key.repeat)
        o->visible = true; /* Escape opens controls, never silently quits. */
    return (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP) &&
        (ev->key.keysym.sym == SDLK_F1 || ev->key.keysym.sym == SDLK_ESCAPE);
}

const char *md_client_phase_label(MdClientPhase phase) {
    switch (phase) {
    case MD_CLIENT_RESOLVE: return "Verifying saved host identity";
    case MD_CLIENT_RELAY: return "Connecting to signaling relays";
    case MD_CLIENT_FIPS: return "FIPS readiness / route";
    case MD_CLIENT_NEGOTIATING: return "Negotiating (relay / authorization)";
    case MD_CLIENT_CONNECTING: return "Connecting";
    case MD_CLIENT_BINDING: return "Verifying session binding";
    case MD_CLIENT_STREAMING: return "Streaming";
    case MD_CLIENT_BACKOFF: return "Reconnect countdown";
    default: return "Disconnected";
    }
}
const char *md_client_error_label(MdClientError error) {
    switch (error) {
    case MD_CLIENT_ERROR_RESOLVE: return "Saved host identity";
    case MD_CLIENT_ERROR_RELAY: return "Signaling relays";
    case MD_CLIENT_ERROR_FIPS: return "FIPS route / daemon";
    case MD_CLIENT_ERROR_ADMISSION: return "Session authorization / relay";
    case MD_CLIENT_ERROR_TRANSPORT: return "Transport connection";
    case MD_CLIENT_ERROR_BINDING: return "Session binding / ACK";
    default: return "None";
    }
}
MdOverlayAction md_overlay_take_action(MdOverlay *o) {
    if (!o) return MD_OVERLAY_NONE;
    MdOverlayAction action = o->action;
    o->action = MD_OVERLAY_NONE;
    return action;
}

void md_overlay_take_host(MdOverlay *o, char pin[65]) {
    std::memcpy(pin, o->quick_pin, sizeof(o->quick_pin));
    std::memset(o->quick_pin, 0, sizeof(o->quick_pin));
}

void md_overlay_reset_session(MdOverlay *o) {
    if (!o) return;
    o->output_changed = o->quality_changed = false;
    o->quality_width = o->quality_height = o->quality_bitrate = o->quality_fps = 0;
    o->smooth_latency = o->smooth_encode = o->smooth_decode = o->smooth_rtt = 0;
    o->smooth_fps = o->smooth_bitrate = 0;
}

static void render_quick_connect(MdOverlay *o, const MdOverlayStats *stats) {
    if (!stats->quick_available) { o->quick_open = false; return; }
    if (ImGui::Button(o->quick_open ? "Close quick connect" : "Quick connect")) {
        o->quick_open = !o->quick_open;
        if (o->quick_open) o->action = MD_OVERLAY_LOAD_HOSTS;
    }
    if (!o->quick_open) return;
    if (stats->quick_loading || o->action == MD_OVERLAY_LOAD_HOSTS) {
        ImGui::TextUnformatted("Reading saved hosts…");
        return;
    }
    if (!stats->quick_hosts) {
        ImGui::TextWrapped("Saved hosts unavailable. Check the client signer and address book.");
        return;
    }
    if (!json_array_size(stats->quick_hosts)) {
        ImGui::TextUnformatted("No saved favorites or recent hosts.");
        return;
    }
    if (stats->quick_configured)
        ImGui::TextUnformatted("Choose a host to connect to:");
    else
        ImGui::TextWrapped("Start with a configured --relay to connect to saved hosts.");
    ImGui::BeginChild("quick-hosts", ImVec2(0, 180), true);
    int group = -1;
    for (size_t i = 0; i < json_array_size(stats->quick_hosts); ++i) {
        const json_t *row = json_array_get(stats->quick_hosts, i);
        const char *pin = json_string_value(json_object_get(row, "pubkey"));
        const char *label = json_string_value(json_object_get(row, "label"));
        const char *alias = json_string_value(json_object_get(row, "nip05"));
        int favorite = json_is_true(json_object_get(row, "favorite"));
        if (favorite != group) {
            ImGui::Separator();
            ImGui::TextUnformatted(favorite ? "Favorites" : "Recent hosts");
            group = favorite;
        }
        bool active = stats->connected && stats->current_pin && !std::strcmp(pin, stats->current_pin);
        ImGui::PushID(pin);
        // Draw untrusted labels as text, not as ImGui identifiers.
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool disabled = active || !stats->quick_configured;
        if (ImGui::Selectable("##connect", active, disabled ? ImGuiSelectableFlags_Disabled : 0,
                              ImVec2(0, 2 * ImGui::GetTextLineHeightWithSpacing()))) {
            std::memcpy(o->quick_pin, pin, sizeof(o->quick_pin));
            o->action = MD_OVERLAY_SWITCH_HOST;
            o->quick_open = false;
        }
        ImVec2 next = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(pos);
        const char *title = label && *label ? label : alias && *alias ? alias : "Saved host";
        ImGui::TextUnformatted(title, title + std::strcspn(title, "\r\n"));
        ImGui::Text("%.4s %.4s %.4s %.4s%s", pin, pin+4, pin+8, pin+12, active ? " — connected" : "");
        ImGui::SetCursorScreenPos(next);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void md_overlay_render(MdOverlay *o, const MdOverlayStats *stats) {
    if (!o || !stats) {
        /* Still need to end the frame even if not rendering stats */
        if (o) {
            ImGui::EndFrame();
        }
        return;
    }

    if (!stats->connected) o->visible = true;
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

        render_draw_data(o);
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
                           "[+] Connected");
    } else if (stats->reconnecting) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "[-] Disconnected - reconnecting...");
        if (stats->reconnect_delay_ms > 0) {
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.5f, 1.0f),
                               "Next retry in %.1fs",
                               (float)stats->reconnect_delay_ms / 1000.0f);
        }
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f),
                           "[-] Disconnected");
    }

    if (stats->connected && stats->outputs && stats->outputs->count && ImGui::CollapsingHeader("Displays")) {
        const MdCaptureOutputs *outputs=stats->outputs;
        if (!outputs->switch_supported || !(stats->granted_caps & MD_CAP_MONITOR))
            ImGui::TextWrapped("Switching unavailable: requires monitor grant; Linux portals require new local consent.");
        for (size_t i=0;i<outputs->count;i++) {
            const MdCaptureOutput *item=&outputs->items[i];
            bool selected=strcmp(item->id,outputs->selected)==0;
            ImGui::PushID((int)i);
            if (ImGui::Selectable(item->name,selected) && outputs->switch_supported && (stats->granted_caps & MD_CAP_MONITOR)) {
                snprintf(o->output,sizeof(o->output),"%s",item->id); o->output_changed=true;
            }
            ImGui::PopID();
        }
    }
    if (stats->connected && stats->quality_supported) {
        if (ImGui::CollapsingHeader("Session quality")) {
            ImGui::Text("Applied: cap %ux%u, %.2f Mbps, %u fps",
                stats->quality.max_width, stats->quality.max_height,
                stats->quality.bitrate / 1000000.0f, stats->quality.fps);
            ImGui::InputInt("Max width (0=host)", &o->quality_width);
            ImGui::InputInt("Max height (0=host)", &o->quality_height);
            ImGui::InputInt("Bitrate bps (0=host)", &o->quality_bitrate, 100000);
            ImGui::InputInt("FPS (0=host)", &o->quality_fps);
            MdQuality q = {(uint32_t)o->quality_width, (uint32_t)o->quality_height,
                           (uint32_t)o->quality_bitrate, (uint32_t)o->quality_fps};
            if (md_quality_valid(&q) && ImGui::Button("Apply quality")) {
                o->quality = q; o->quality_changed = true;
            }
        }
    }
    render_quick_connect(o, stats);
    ImGui::TextWrapped("Phase: %s", md_client_phase_label(stats->phase));
    if (stats->error != MD_CLIENT_ERROR_NONE)
        ImGui::TextWrapped("Error: %s", md_client_error_label(stats->error));
    if (stats->connected) {
        ImGui::Text("Granted capabilities:");
        ImGui::Text("Video: %s  Agent: %s  Input: %s",
                    stats->granted_caps & MD_CAP_VIDEO ? "yes" : "no",
                    stats->granted_caps & MD_CAP_AGENT ? "yes" : "no",
                    stats->granted_caps & MD_CAP_INPUT ? "yes" : "no");
    }
    if (stats->status_message && stats->status_message[0]) {
        ImGui::TextWrapped("%s", stats->status_message);
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

    if (stats->paused) {
        if (ImGui::Button("Connect / Retry")) o->action = MD_OVERLAY_RETRY;
    } else {
        if (ImGui::Button(stats->connected ? "Disconnect" : "Cancel connection"))
            o->action = MD_OVERLAY_DISCONNECT;
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit")) o->action = MD_OVERLAY_QUIT;

    ImGui::PopStyleColor(3);

    /* ── Footer hint ─────────────────────────────────────── */
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 0.7f),
                       "F1: toggle overlay | Escape: controls");

    ImGui::End();

    render_draw_data(o);
}

bool md_overlay_wants_mouse(MdOverlay *o) {
    if (!o) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool md_overlay_wants_keyboard(MdOverlay *o) {
    if (!o) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}

void md_overlay_destroy(MdOverlay *o) {
    if (!o) return;

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    std::free(o);
}

} /* extern "C" */

bool md_overlay_take_quality(MdOverlay *o, MdQuality *quality) {
    if (!o || !quality || !o->quality_changed) return false;
    *quality = o->quality; o->quality_changed = false; return true;
}

bool md_overlay_take_output(MdOverlay *o,char output[32]) {
    if (!o || !output || !o->output_changed) return false;
    snprintf(output,32,"%s",o->output); o->output_changed=false; return true;
}
