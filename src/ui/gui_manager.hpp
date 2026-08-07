#pragma once

#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include "../config.hpp"
#include "../dsp/frame.hpp"
#include "sdl_display.hpp"

namespace famidec {

class GuiManager {
public:
    // Called once after SDL window/renderer are created.
    void init(SDL_Window* win, SDL_Renderer* ren, const Config& cfg);
    // Called once before shutdown.
    void shutdown();
    // Called every frame with current decoder stats.
    float render(const OsdStats& stats);

private:
    void render_control_panel(const OsdStats& stats);

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    Config cfg_;  // stored at init for render-time use
};

} // namespace famidec
