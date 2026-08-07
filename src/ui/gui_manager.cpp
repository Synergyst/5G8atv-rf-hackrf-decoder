#define SDL_MAIN_HANDLED
#include "gui_manager.hpp"
#include <cmath>
#include <cstdio>

#include <imgui_impl_sdlrenderer2.h>

namespace famidec {

void GuiManager::init(SDL_Window* win, SDL_Renderer* ren) {
    win_ = win;
    ren_ = ren;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren_);
    ImGui_ImplSDLRenderer2_Init(ren_);
}

void GuiManager::shutdown() {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void GuiManager::render_control_panel(const OsdStats& stats) {
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::Begin("Control Panel", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_AlwaysAutoResize);

    // -- Channel & Frequency --
    if (show_channel_info_) {
        ImGui::TextColored(ImVec4(0.16f, 1.0f, 0.31f, 1.0f),
                           "Channel: %s",
                           stats.channel.empty() ? "---" : stats.channel.c_str());
        char freq_buf[32];
        std::snprintf(freq_buf, sizeof(freq_buf), "%.2f MHz", stats.freq_mhz);
        ImGui::SameLine();
        ImGui::TextDisabled(freq_buf);
    }

    // -- Sync Status --
    if (show_sync_status_) {
        if (stats.line_locked && stats.vsync_locked) {
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f),
                               "SYNC: OK  %0.1f FPS", stats.fps);
        } else if (stats.vsync_locked) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                               "SYNC: LOST LINE");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.0f, 1.0f),
                               "SYNC: SEARCHING...");
        }
    }

    // -- Signal Quality Bars --
    if (show_signal_bars_) {
        float bar_width = 200.0f;
        float bar_height = 10.0f;

        // Ring buffer fill
        ImGui::Text("Ring Buffer:");
        ImGui::ProgressBar(stats.ring_fill / 100.0f, ImVec2(bar_width, bar_height));
        char ring_label[32];
        std::snprintf(ring_label, sizeof(ring_label), "%.0f%%", stats.ring_fill);
        ImGui::SameLine();
        ImGui::Text(ring_label);

        // Burst amplitude (color strength)
        float burst_norm = std::min(1.0f, stats.burst_amp / 10.0f);
        ImGui::Text("Chroma:");
        ImGui::ProgressBar(burst_norm, ImVec2(bar_width, bar_height));
        char burst_label[32];
        std::snprintf(burst_label, sizeof(burst_label), "%.1f", stats.burst_amp);
        ImGui::SameLine();
        ImGui::Text(burst_label);

        // Clipping indicator
        if (stats.clipping) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "!! CLIPPING !!");
        }
    }

    // -- AGC Info --
    if (show_agc_info_) {
        if (stats.gain_auto) {
            ImGui::TextColored(ImVec4(0.0f, 0.7f, 1.0f, 1.0f),
                               "AGC: AUTO");
        } else {
            char agc_label[64];
            std::snprintf(agc_label, sizeof(agc_label),
                          "AGC: LNA %d  VGA %d  AMP %s",
                          stats.lna, stats.vga,
                          stats.amp ? "ON" : "OFF");
            ImGui::Text(agc_label);
        }
    }

    // -- Stats --
    char stats_label[128];
    std::snprintf(stats_label, sizeof(stats_label),
                  "Frames: %llu  Dropped: %llu  Clipped: %s  Latency: %.0fms",
                  static_cast<unsigned long long>(stats.frames),
                  static_cast<unsigned long long>(stats.dropped),
                  stats.clipping ? "YES" : "NO",
                  stats.video_latency_ms);
    ImGui::TextDisabled(stats_label);

    // -- Screenshot Button --
    if (ImGui::Button("Screenshot", ImVec2(-1, 0))) {
        screenshot_requested_ = 1.0f;
    }

    ImGui::Separator();

    // -- Toggle Checkboxes (GUI-only settings) --
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 2));
    ImGui::Checkbox("Channel Info", &show_channel_info_);
    ImGui::Checkbox("Sync Status", &show_sync_status_);
    ImGui::Checkbox("Signal Bars", &show_signal_bars_);
    ImGui::Checkbox("AGC Info", &show_agc_info_);
    ImGui::PopStyleVar();

    ImGui::End();
}

void GuiManager::render_signal_bars(const OsdStats& stats) {
    // Signal bars are now rendered inline in render_control_panel()
    // This function is kept for backwards compatibility
    (void)stats;
}

float GuiManager::render(const OsdStats& stats) {
    screenshot_requested_ = 0.0f;

    // Pass SDL events to ImGui
    ImGui_ImplSDL2_NewFrame();
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui::NewFrame();

    render_control_panel(stats);

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren_);

    return screenshot_requested_;
}

} // namespace famidec
