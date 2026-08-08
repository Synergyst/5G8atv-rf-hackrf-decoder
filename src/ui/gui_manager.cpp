#define SDL_MAIN_HANDLED
#include "gui_manager.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui_impl_sdlrenderer2.h>

namespace famidec {

// Helper: if cfg values >= 0, use them as-is (0..1 range).
// If any is negative, fall back to the semantic color.
static ImVec4 overlay_color(const Config& cfg, float sr, float sg, float sb) {
    if (cfg.overlay_color_r >= 0.0f && cfg.overlay_color_g >= 0.0f && cfg.overlay_color_b >= 0.0f) {
        return ImVec4(cfg.overlay_color_r, cfg.overlay_color_g, cfg.overlay_color_b, 1.0f);
    }
    return ImVec4(sr, sg, sb, 1.0f);
}

void GuiManager::init(SDL_Window* win, SDL_Renderer* ren, const Config& cfg) {
    win_ = win;
    ren_ = ren;
    cfg_ = cfg;  // store for render-time use

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // True overlay: no chrome, transparent background, compact.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding       = ImVec2(8, 6);
    style.WindowRounding      = 4.0f;
    style.WindowBorderSize    = 0.0f;
    style.FramePadding        = ImVec2(4, 2);
    style.FrameRounding       = 2.0f;
    style.FrameBorderSize     = 0.0f;
    style.GrabRounding        = 2.0f;
    style.GrabMinSize         = 10.0f;
    style.ItemSpacing         = ImVec2(6, 3);
    style.ItemInnerSpacing    = ImVec2(4, 0);
    style.IndentSpacing       = 6.0f;

    // Zero-alpha window bg = video shows through completely.
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_Border]             = ImVec4(0.4f, 0.6f, 0.8f, 0.3f);
    c[ImGuiCol_BorderShadow]       = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBgHovered]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBgActive]      = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TitleBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TitleBgActive]      = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_MenuBarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarBg]        = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab]      = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_CheckMark]          = ImVec4(0.16f, 1.0f, 0.31f, 1.0f);
    c[ImGuiCol_SliderGrab]         = ImVec4(0.16f, 1.0f, 0.31f, 1.0f);
    c[ImGuiCol_SliderGrabActive]   = ImVec4(0.2f, 1.0f, 0.4f, 1.0f);
    c[ImGuiCol_Button]             = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ButtonHovered]      = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ButtonActive]       = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // Font: configurable size, OversampleH=2 for bold/thick look.
    ImFontConfig fontCfg;
    fontCfg.SizePixels = static_cast<float>(cfg_.overlay_font_size);
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 1;
    fontCfg.PixelSnapH = true;
    io.Fonts->AddFontDefault(&fontCfg);

    ImGui_ImplSDL2_InitForSDLRenderer(win, ren_);
    ImGui_ImplSDLRenderer2_Init(ren_);
}

void GuiManager::shutdown() {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void GuiManager::render_control_panel(const OsdStats& stats) {
    auto vp = ImGui::GetMainViewport();
    int mx = cfg_.overlay_margin_x;
    int my = cfg_.overlay_margin_y;

    float posX = static_cast<float>(mx);
    float posY;
    if (cfg_.overlay_position == Config::OverlayPos::Bottom) {
        posY = vp->WorkSize.y - 16 - my;
    } else {
        posY = static_cast<float>(my);
    }

    ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);

    ImVec2 avail = ImVec2(vp->WorkSize.x - 16 - mx * 2,
                          vp->WorkSize.y - 16 - my * 2);
    ImVec2 min_size(260, 110);
    ImVec2 max_size(avail.x < min_size.x ? min_size.x : avail.x,
                    avail.y < min_size.y ? min_size.y : avail.y);
    ImGui::SetNextWindowSizeConstraints(min_size, max_size);

    ImGui::Begin("Control Panel", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Pre-compute colors.
    const ImVec4 col_ch    = overlay_color(cfg_, 0.16f, 1.0f, 0.31f);
    const ImVec4 col_sync_ok   = overlay_color(cfg_, 0.0f, 1.0f, 0.0f);
    const ImVec4 col_sync_warn = overlay_color(cfg_, 1.0f, 0.8f, 0.0f);
    const ImVec4 col_sync_no   = overlay_color(cfg_, 1.0f, 0.3f, 0.0f);
    const ImVec4 col_agc_auto  = overlay_color(cfg_, 0.0f, 0.7f, 1.0f);
    const ImVec4 col_clkin     = overlay_color(cfg_, 0.0f, 0.8f, 0.0f);
    const ImVec4 col_clip      = overlay_color(cfg_, 1.0f, 0.2f, 0.2f);
    const ImVec4 col_dim       = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    const ImVec4 col_muted     = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);

    // Channel & Frequency
    {
        ImGui::PushStyleColor(ImGuiCol_Text, col_ch);
        const char* ch = stats.channel.empty() ? "---" : stats.channel.c_str();
        char freq_buf[32];
        std::snprintf(freq_buf, sizeof(freq_buf), "%.2f MHz", stats.freq_mhz);
        ImGui::Text("[%s] %s", ch, freq_buf);
        ImGui::PopStyleColor();
    }

    // Sync Status
    {
        if (stats.line_locked && stats.vsync_locked) {
            ImGui::PushStyleColor(ImGuiCol_Text, col_sync_ok);
            ImGui::Text("[SYNC] Line/Frame locked");
            //ImGui::Text("[SYNC] Line/Frame locked  %.1f FPS", stats.fps);
        } else if (stats.vsync_locked) {
            ImGui::PushStyleColor(ImGuiCol_Text, col_sync_warn);
            ImGui::Text("[SYNC] Frame locked -- line lost");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, col_sync_no);
            ImGui::Text("[SYNC] Searching...");
        }
        ImGui::PopStyleColor();
    }

    // Signal Quality (ring buffer, chroma, clipping)
    if (cfg_.show_signal) {
        float bar_w = 150.0f;
        float bar_h = 10.0f;
        char buf[32];

        ImGui::Text("[SIGNAL] Ring buffer:");
        ImGui::SameLine();
        ImGui::ProgressBar(std::clamp(stats.ring_fill / 100.0f, 0.0f, 1.0f),
                           ImVec2(bar_w, bar_h), nullptr);
        std::snprintf(buf, sizeof(buf), "%.0f%%", stats.ring_fill);
        ImGui::SameLine();
        ImGui::TextColored(col_muted, "%s", buf);

        ImGui::Text("  Chroma:");
        ImGui::SameLine();
        ImGui::ProgressBar(std::clamp(stats.burst_amp / 20.0f, 0.0f, 1.0f),
                           ImVec2(bar_w, bar_h), nullptr);
        std::snprintf(buf, sizeof(buf), "%.1f", stats.burst_amp);
        ImGui::SameLine();
        ImGui::TextColored(col_muted, "%s", buf);

        if (stats.clipping) {
            ImGui::PushStyleColor(ImGuiCol_Text, col_clip);
            ImGui::Text("  ** CLIPPING **");
            ImGui::PopStyleColor();
        }
    }

    // AGC Info (manual gain readout)
    if (cfg_.show_agc) {
        if (stats.gain_auto) {
            ImGui::PushStyleColor(ImGuiCol_Text, col_agc_auto);
            ImGui::Text("[AGC] AUTO");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, col_dim);
            ImGui::Text("[AGC] LNA %2d  VGA %2d  AMP %s",
                        stats.lna, stats.vga,
                        stats.amp ? "ON" : "OFF");
            ImGui::PopStyleColor();
        }
    }

    // CLKIN Status (external clock locked)
    if (cfg_.show_clkin && stats.clkin_locked) {
        ImGui::PushStyleColor(ImGuiCol_Text, col_clkin);
        ImGui::Text("[CLKIN] LOCKED");
        ImGui::PopStyleColor();
    }

    // Stats (frames, dropped, clipping, latency)
    if (cfg_.show_stats) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "Frames: %llu  Dropped: %llu  Clipped: %s  Latency: %.0f ms",
                      static_cast<unsigned long long>(stats.frames),
                      static_cast<unsigned long long>(stats.dropped),
                      stats.clipping ? "YES" : "NO",
                      stats.video_latency_ms);
        ImGui::PushStyleColor(ImGuiCol_Text, col_dim);
        ImGui::Text("%s", buf);
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

float GuiManager::render(const OsdStats& stats) {
    ImGui_ImplSDL2_NewFrame();
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui::NewFrame();

    render_control_panel(stats);

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren_);

    return 0.0f;
}

} // namespace famidec
