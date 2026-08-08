#pragma once

#include <string>
#include <SDL2/SDL.h>
#include <vector>

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

struct CrtLut {
    struct Entry {
        int32_t src;
        uint16_t gain;
    };
    int width = 0;
    int height = 0;
    std::vector<Entry> map;

    CrtLut() = default;

    void rebuild(int w, int h) {
        width = w;
        height = h;
        map.resize(static_cast<size_t>(w) * h);
        const double cx = w / 2.0, cy = h / 2.0;
        const double k1 = 0.055;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double nx = (x - cx) / cx, ny = (y - cy) / cy;
                double r2 = nx * nx + ny * ny;
                double f = 1.0 + k1 * r2;
                int sx = static_cast<int>(cx + nx * f * cx + 0.5);
                int sy = static_cast<int>(cy + ny * f * cy + 0.5);
                Entry& e = map[static_cast<size_t>(y) * w + x];
                if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
                    e.src = -1; e.gain = 0;
                } else {
                    e.src = sy * w + sx;
                    double vig = 1.0 - 0.18 * r2 * r2;
                    double scan = (sy & 1) ? 0.72 : 1.0;
                    e.gain = static_cast<uint16_t>(std::max(0.0, vig * scan) * 256.0 + 0.5);
                }
            }
        }
    }
};

void apply_crt(const Frame& in, Frame& out, const CrtLut& lut);

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
    // Initialize with specified dimensions. Returns false if SDL init fails.
    bool init(const std::string& title, int width, int height);
    ~SdlDisplay();

    KeyAction poll();
    void render(const Frame* frame, const OsdStats& stats);
    void render_video_only(const Frame* frame);
    bool screenshot(const Frame& frame, const std::string& path);
    void set_hotkeys_enabled(bool enabled) { hotkeys_enabled_ = enabled; }

    SDL_Window* win() const { return win_; }
    SDL_Renderer* renderer() const { return ren_; }

    // Expose CRT LUT rebuild for main.cpp
    void rebuild_crt_lut(int w, int h) { crt_lut_.rebuild(w, h); }

private:
    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    int win_width_ = 640;
    int win_height_ = 480;
    Frame last_frame_;
    Frame osd_frame_;
    Frame crt_frame_;
    bool have_frame_ = false;
    bool hotkeys_enabled_ = true;
    CrtLut crt_lut_;
};

} // namespace famidec
