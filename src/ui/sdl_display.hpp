#pragma once

#include <string>
#include <SDL2/SDL.h>

#include "../dsp/frame.hpp"

namespace famidec {

struct OsdStats {
    bool line_locked = false;
    bool vsync_locked = false;
    float burst_amp = 0.0f;
    float ring_fill = 0.0f;
    uint64_t dropped = 0;
    uint64_t clipped = 0;
    uint64_t frames = 0;
    uint64_t lines = 0;
    int lna = 0, vga = 0;
    bool amp = false;
    bool gain_auto = true;
    bool clipping = false;
    bool clkin_locked = false;  // external clock signal detected
    double freq_mhz = 0.0;
    std::string channel;
    float fps = 0.0f;
    float video_latency_ms = 0.0f;
    bool show_osd = true;
    bool show_help = false;
    bool crt = false;
    bool recording = false;
    float rec_seconds = 0.0f;
};

enum class KeyAction {
    None,
    Quit,
    GainLnaUp,
    GainLnaDown,
    GainVgaUp,
    GainVgaDown,
    ToggleColor,
    Screenshot,
    ToggleHelp,
    FreqUp,
    FreqDown,
    FreqUpBig,
    FreqDownBig,
    ToggleCrt,
    ToggleRecord,
    ToggleGainMode,
    ToggleAmp,
    ToggleOsd,
};

class SdlDisplay {
public:
    bool init(const std::string& title);
    ~SdlDisplay();

    KeyAction poll();
    void render(const Frame* frame, const OsdStats& stats);
    void render_video_only(const Frame* frame);
    bool screenshot(const Frame& frame, const std::string& path);
    void set_hotkeys_enabled(bool enabled) { hotkeys_enabled_ = enabled; }

    SDL_Window* win() const { return win_; }
    SDL_Renderer* renderer() const { return ren_; }

private:
    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    Frame last_frame_;
    Frame osd_frame_;
    Frame crt_frame_;
    bool have_frame_ = false;
    bool hotkeys_enabled_ = true;
};

} // namespace famidec
