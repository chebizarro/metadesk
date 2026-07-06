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
    bool           show_allowlist;       /* allowlist panel toggled on */
    bool           show_peers;           /* peer list panel toggled on */
    SDL_Window    *window;
    SDL_Renderer  *renderer;

    /* Smoothed stats for display (avoid jitter) */
    float          smooth_latency;
    float          smooth_encode;
    float          smooth_decode;
    float          smooth_rtt;
    float          smooth_fps;
    float          smooth_bitrate;

    /* Allowlist add form state */
    char           add_npub_buf[130];    /* input: hex npub to add */
    char           add_caps_buf[64];     /* input: capabilities    */

    /* Approval popup state */
    bool           approval_add_to_allowlist; /* checkbox: also add to allowlist */
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
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(o->renderer);
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
    } else if (stats->reconnecting) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "\xE2\x97\x8B Disconnected \xE2\x80\x94 reconnecting...");  /* ○ — */
        if (stats->reconnect_delay_ms > 0) {
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.5f, 1.0f),
                               "Next retry in %.1fs",
                               (float)stats->reconnect_delay_ms / 1000.0f);
        }
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f),
                           "\xE2\x97\x8B Disconnected");  /* ○ */
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

    float button_width = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Disconnect", ImVec2(button_width, 0))) {
        o->disconnect_requested = true;
        /* Post a quit event so the main loop exits cleanly */
        SDL_Event quit_event;
        quit_event.type = SDL_QUIT;
        SDL_PushEvent(&quit_event);
    }

    ImGui::PopStyleColor(3);

    /* ── Peer list toggle ─────────────────────────────────── */
    if (stats->peers && stats->peer_count > 0) {
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button(o->show_peers ? "Hide Peers" : "Peers",
                          ImVec2(button_width, 0))) {
            o->show_peers = !o->show_peers;
        }
    }

    /* ── Allowlist toggle ─────────────────────────────────── */
    if (stats->allowlist_entries || stats->on_allowlist_add) {
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button(o->show_allowlist ? "Hide Allowlist" : "Allowlist",
                          ImVec2(button_width, 0))) {
            o->show_allowlist = !o->show_allowlist;
        }
    }

    /* ── Footer hint ─────────────────────────────────────── */
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 0.7f),
                       "F1: toggle overlay");

    ImGui::End();

    /* ── Peer list panel (separate window) ────────────────── */
    if (o->show_peers && stats->peers && stats->peer_count > 0) {
        ImGui::SetNextWindowPos(ImVec2(280, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);

        ImGui::Begin("Connected Peers", &o->show_peers,
                     ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Peers: %d", stats->peer_count);
        ImGui::Separator();

        if (ImGui::BeginTable("##peers", 4,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("npub", 0, 3.0f);
            ImGui::TableSetupColumn("Status", 0, 1.2f);
            ImGui::TableSetupColumn("RTT", 0, 0.8f);
            ImGui::TableSetupColumn("Caps", 0, 1.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < stats->peer_count; i++) {
                const MdOverlayPeerInfo *p = &stats->peers[i];
                ImGui::TableNextRow();

                /* npub column — truncate */
                ImGui::TableNextColumn();
                if (p->pubkey_hex && std::strlen(p->pubkey_hex) >= 16) {
                    char trunc[24];
                    std::snprintf(trunc, sizeof(trunc), "%.8s...%.8s",
                                  p->pubkey_hex,
                                  p->pubkey_hex + std::strlen(p->pubkey_hex) - 8);
                    ImGui::TextUnformatted(trunc);
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("npub: %s", p->pubkey_hex);
                        if (p->session_id)
                            ImGui::Text("Session: %s", p->session_id);
                        ImGui::EndTooltip();
                    }
                } else {
                    ImGui::TextUnformatted(p->pubkey_hex ? p->pubkey_hex : "?");
                }

                /* Status column — color-coded */
                ImGui::TableNextColumn();
                if (p->status) {
                    ImVec4 status_color;
                    if (std::strcmp(p->status, "active") == 0)
                        status_color = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
                    else if (std::strcmp(p->status, "negotiating") == 0)
                        status_color = ImVec4(0.9f, 0.9f, 0.2f, 1.0f);
                    else
                        status_color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                    ImGui::TextColored(status_color, "%s", p->status);
                } else {
                    ImGui::TextUnformatted("?");
                }

                /* RTT column */
                ImGui::TableNextColumn();
                if (p->rtt_ms > 0.0f) {
                    ImVec4 rtt_color;
                    if (p->rtt_ms < 50.0f)
                        rtt_color = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
                    else if (p->rtt_ms < 150.0f)
                        rtt_color = ImVec4(0.9f, 0.9f, 0.2f, 1.0f);
                    else
                        rtt_color = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
                    ImGui::TextColored(rtt_color, "%.0f ms", p->rtt_ms);
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "—");
                }

                /* Caps column */
                ImGui::TableNextColumn();
                if (p->capabilities) {
                    /* Decode capability bits into short labels */
                    char caps_str[64] = {0};
                    int pos = 0;
                    if (p->capabilities & 0x01) pos += std::snprintf(caps_str + pos, sizeof(caps_str) - pos, "V"); /* view */
                    if (p->capabilities & 0x02) pos += std::snprintf(caps_str + pos, sizeof(caps_str) - pos, "K"); /* keyboard */
                    if (p->capabilities & 0x04) pos += std::snprintf(caps_str + pos, sizeof(caps_str) - pos, "M"); /* mouse */
                    if (p->capabilities & 0x08) pos += std::snprintf(caps_str + pos, sizeof(caps_str) - pos, "A"); /* a11y */
                    if (pos == 0) std::snprintf(caps_str, sizeof(caps_str), "none");
                    ImGui::TextUnformatted(caps_str);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("V=view K=keyboard M=mouse A=a11y (0x%04x)",
                                          p->capabilities);
                    }
                } else {
                    ImGui::TextUnformatted("all");
                }
            }
            ImGui::EndTable();
        }

        ImGui::End();
    }

    /* ── Allowlist panel (separate window) ────────────────── */
    if (o->show_allowlist && (stats->allowlist_entries || stats->on_allowlist_add)) {
        ImGui::SetNextWindowPos(ImVec2(280, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);

        ImGui::Begin("Allowlist (NIP-51)", &o->show_allowlist,
                     ImGuiWindowFlags_AlwaysAutoResize);

        /* Current entries */
        if (stats->allowlist_count > 0 && stats->allowlist_entries) {
            ImGui::Text("Allowed peers: %d", stats->allowlist_count);
            ImGui::Separator();

            if (ImGui::BeginTable("##allowlist", 3,
                                  ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("npub", 0, 3.0f);
                ImGui::TableSetupColumn("Caps", 0, 1.5f);
                ImGui::TableSetupColumn("##remove", 0, 0.5f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < stats->allowlist_count; i++) {
                    const MdOverlayAllowlistEntry *e = &stats->allowlist_entries[i];
                    ImGui::TableNextRow();

                    /* npub column — truncate to first/last 8 chars */
                    ImGui::TableNextColumn();
                    if (e->pubkey_hex && std::strlen(e->pubkey_hex) >= 16) {
                        char trunc[24];
                        std::snprintf(trunc, sizeof(trunc), "%.8s...%.8s",
                                      e->pubkey_hex,
                                      e->pubkey_hex + std::strlen(e->pubkey_hex) - 8);
                        ImGui::TextUnformatted(trunc);
                        /* Tooltip with full npub on hover */
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", e->pubkey_hex);
                    } else {
                        ImGui::TextUnformatted(e->pubkey_hex ? e->pubkey_hex : "?");
                    }

                    /* Caps column */
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e->caps ? e->caps : "(all)");

                    /* Remove button */
                    ImGui::TableNextColumn();
                    if (stats->on_allowlist_remove) {
                        ImGui::PushID(i);
                        ImGui::PushStyleColor(ImGuiCol_Button,
                                              ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                              ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                              ImVec4(0.4f, 0.05f, 0.05f, 1.0f));
                        if (ImGui::SmallButton("\xC3\x97")) {  /* × */
                            stats->on_allowlist_remove(e->pubkey_hex,
                                                      stats->allowlist_userdata);
                        }
                        ImGui::PopStyleColor(3);
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                               "No allowlist entries (open mode)");
        }

        /* Add entry form */
        if (stats->on_allowlist_add) {
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Add peer:");

            ImGui::SetNextItemWidth(280);
            ImGui::InputTextWithHint("##npub", "npub hex (64 chars)",
                                     o->add_npub_buf, sizeof(o->add_npub_buf));

            ImGui::SetNextItemWidth(140);
            ImGui::SameLine();
            ImGui::InputTextWithHint("##caps", "caps",
                                     o->add_caps_buf, sizeof(o->add_caps_buf));

            ImGui::SameLine();
            bool valid = std::strlen(o->add_npub_buf) == 64;
            if (!valid) {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
                ImGui::Button("Add");
                ImGui::PopStyleVar();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.1f, 0.5f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(0.05f, 0.4f, 0.05f, 1.0f));
                if (ImGui::Button("Add")) {
                    const char *caps = o->add_caps_buf[0] ? o->add_caps_buf : NULL;
                    stats->on_allowlist_add(o->add_npub_buf, caps,
                                            stats->allowlist_userdata);
                    o->add_npub_buf[0] = '\0';
                    o->add_caps_buf[0] = '\0';
                }
                ImGui::PopStyleColor(3);
            }
        }

        ImGui::End();
    }

    /* ── Approval popup (modal) ──────────────────────────── */
    if (stats->pending_approval && stats->on_approval) {
        const MdOverlayApprovalRequest *req = stats->pending_approval;
        const char *popup_id = "Session Request##approval";

        ImGui::OpenPopup(popup_id);

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Appearing);

        if (ImGui::BeginPopupModal(popup_id, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "\xE2\x9A\xa0 Unknown peer requesting session");  /* ⚠ */
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* Requester info */
            ImGui::Text("npub:");
            ImGui::SameLine();
            if (req->pubkey_hex && std::strlen(req->pubkey_hex) >= 16) {
                char trunc[24];
                std::snprintf(trunc, sizeof(trunc), "%.8s...%.8s",
                              req->pubkey_hex,
                              req->pubkey_hex + std::strlen(req->pubkey_hex) - 8);
                ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", trunc);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", req->pubkey_hex);
            } else {
                ImGui::TextUnformatted(req->pubkey_hex ? req->pubkey_hex : "?");
            }

            if (req->fips_addr && req->fips_addr[0]) {
                ImGui::Text("Address:");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                                   "%s", req->fips_addr);
            }

            /* Requested capabilities */
            ImGui::Spacing();
            ImGui::Text("Requested capabilities:");
            ImGui::Indent(10.0f);
            if (req->requested_caps & 0x01)
                ImGui::BulletText("View (screen capture)");
            if (req->requested_caps & 0x02)
                ImGui::BulletText("Keyboard input");
            if (req->requested_caps & 0x04)
                ImGui::BulletText("Mouse input");
            if (req->requested_caps & 0x08)
                ImGui::BulletText("Accessibility tree");
            if (req->requested_caps == 0)
                ImGui::BulletText("(all capabilities)");
            ImGui::Unindent(10.0f);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* Add-to-allowlist checkbox */
            ImGui::Checkbox("Remember (add to allowlist)",
                            &o->approval_add_to_allowlist);

            ImGui::Spacing();

            /* Allow / Deny buttons */
            float avail = ImGui::GetContentRegionAvail().x;
            float btn_w = (avail - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.1f, 0.5f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.05f, 0.4f, 0.05f, 1.0f));
            if (ImGui::Button("Allow", ImVec2(btn_w, 0))) {
                stats->on_approval(req->pubkey_hex, true,
                                   o->approval_add_to_allowlist,
                                   stats->approval_userdata);
                o->approval_add_to_allowlist = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.4f, 0.05f, 0.05f, 1.0f));
            if (ImGui::Button("Deny", ImVec2(btn_w, 0))) {
                stats->on_approval(req->pubkey_hex, false, false,
                                   stats->approval_userdata);
                o->approval_add_to_allowlist = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);

            ImGui::EndPopup();
        }
    }

    /* Finalize and render */
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
    SDL_RenderPresent(o->renderer);
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
