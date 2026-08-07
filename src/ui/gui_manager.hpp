#pragma once

#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include "../dsp/frame.hpp"
#include "sdl_display.hpp"

namespace famidec {

class GuiManager {
public:
    // Called once after SDL window/renderer are created.
    void init(SDL_Window* win, SDL_Renderer* ren);
    // Called once before shutdown.
    void shutdown();
    // Called every frame with current decoder stats.
    // Returns 1.0f if screenshot was requested via GUI.
    float render(const OsdStats& stats);

private:
    void render_control_panel(const OsdStats& stats);
    void render_signal_bars(const OsdStats& stats);

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;

    // UI state
    bool show_channel_info_ = true;
    bool show_sync_status_ = true;
    bool show_signal_bars_ = true;
    bool show_agc_info_ = true;

    // Actions triggered by GUI
    float screenshot_requested_ = 0.0f;
};

} // namespace famidec
