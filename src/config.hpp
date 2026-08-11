#pragma once

#include <string>
#include <vector>
#include <mutex>

namespace famidec {

// ── Dynamic Config Event System ──────────────────────────────────────────────
// Thread-safe queue for config changes from Web GUI to DSP thread.

enum ConfigChangeType {
    CFG_FM_DEV,
    CFG_INVERT,
    CFG_SATURATION,
    CFG_HUE_DEG,
    CFG_OVERSCAN,
    CFG_VIDEO_LPF,
    CFG_AFC,
    CFG_DENOISE,
    CFG_DENOISE_TEMPORAL,
    CFG_DENOISE_MEDIAN,
    CFG_DENOINE_MEDIAN_STRENGTH,
    CFG_SAMPLE_RATE,
    CFG_VIDEO_CARRIER,
    CFG_OFFSET_HZ,
    CFG_LNA_GAIN,
    CFG_VGA_GAIN,
    CFG_AMP,
    CFG_GAIN_AUTO,
    CFG_FRAME_WIDTH,
    CFG_FRAME_HEIGHT,
    CFG_AUTO_DETECT,
    CFG_CLkout,
    CFG_ENFORCE_CLKIN,
};

struct ConfigChangeEvent {
    ConfigChangeType type;
    union {
        int int_val;
        double dbl_val;
        bool bool_val;
        float flt_val;
    } val;
};

class ConfigChangeQueue {
public:
    void push(const ConfigChangeEvent& event) {
        std::lock_guard<std::mutex> lk(mu_);
        if (events_.size() < 100) events_.push_back(event);
    }

    void push(const std::vector<ConfigChangeEvent>& src_events) {
        for (auto& e : src_events) {
            if (events_.size() < 100) events_.push_back(e);
        }
    }

    bool has_events() {
        std::lock_guard<std::mutex> lk(mu_);
        return !events_.empty();
    }

    bool pop(ConfigChangeEvent& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (events_.empty()) return false;
        out = events_.front();
        events_.erase(events_.begin());
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        events_.clear();
    }

private:
    std::mutex mu_;
    std::vector<ConfigChangeEvent> events_;
};

struct Config {
    // RF
    double video_carrier_hz = 5.800e9;  // 5.8 GHz FPV band F, channel 4
    double sample_rate = 10e6;          // FPV FM video fits in +-4.5 MHz
    int sample_bits = 8;                // DSP/source IQ width: 8 or 16 bits
                                        // (measured: <0.3% energy outside);
                                        // halves CPU and USB load vs 20 MSPS
    double offset_hz = 0.0;             // no offset tuning: DC spike removed by
                                        // the DcBlocker (band too wide to offset)
    int lna_gain = 40;         // 0-40, step 8. Max: FPV signals are weak
                               // and the LNA is what buys range (the VGA
                               // only scales; use --amp for more)
    int vga_gain = 20;         // 0-62, step 2
    bool amp = true;           // +14 dB RF preamp; FPV signals are weak
                               // (--no-amp or the b key to disable)
    bool gain_auto = true;     // auto LNA/VGA from ADC peak + clip stats;
                               // manual via --gain manual, explicit
                               // --lna/--vga, or the l/g keys. The RF amp
                               // is never auto-switched (intermod risk).

    enum class Input { HackRF, File, SoapySDR, UHD };
    Input input = Input::HackRF;
    std::string file_path;
    bool loop = false;
    std::string soapysdr_device_args;  // device selection for SoapySDR
    std::string uhd_device_args;       // device selection for native UHD
    double uhd_gain_db = 45.0;         // aggregate UHD RX gain
    std::string uhd_antenna;           // optional UHD RX antenna name

    enum class Mode { Color, Gray };
    Mode mode = Mode::Color;

    // FM-ATV parameters.
    double fm_dev_hz = 5.0e6;       // FM peak deviation (--dev)
    bool invert = false;            // --invert: flip discriminator polarity
                                    // for non-standard VTX
    bool afc = true;                // auto re-tune onto the VTX center
                                    // (--no-afc to disable)
    double video_lpf_hz = 0.0;      // post-detector real LPF, 0 = off.
                                    // At 10 MSPS the FM audio subcarrier is
                                    // outside Nyquist, so this is purely an
                                    // optional noise filter (--lpf 4.2e6
                                    // limits to the NTSC video bandwidth)

    std::string record_path;          // tee raw IQ to .cs8 while decoding
    std::string dump_composite_path;  // dump post-AGC composite as f32
    bool spectrum = false;            // PSD printout mode, no video
    bool headless = false;            // decode without SDL window (dump frames)
    std::string dump_frames_prefix;   // write decoded frames as PPM
    int dump_frame_count = 0;

    // Color trims
    float saturation = 1.0f;
    float hue_deg = 0.0f;

    // Post-frame denoise pipeline.
    // Spatial: 3×3 median on Y (removes isolated snow pixels)
    // Temporal median: N-frame median on Y (removes horizontal static lines)
    // Temporal IIR: weak, legacy
    float denoise = 0.0f;             // spatial 3×3 median strength, 0.0=off
    float denoise_temporal = 0.0f;    // temporal IIR strength (legacy, weak)
    int denoise_temporal_median = 0;  // N-frame temporal median, 0=off, 3-9 (odd)
    float denoise_temporal_median_strength = 1.0f;  // blend strength 0..1

    // Horizontal overscan crop, fraction of the active line removed from
    // EACH side. 0 = show the full active line: FPV cameras put OSD text
    // right at the edges (TV-style hiding was 0.047).
    float overscan = 0.0f;

    // Output resolution and aspect ratio.
    int frame_width = 640;
    int frame_height = 480;
    enum class AspectRatio {
        Custom,    // user-defined WxH
        R4_3,      // 4:3 — standard for NTSC/PAL
        R16_9,     // 16:9 — widescreen
        R16_10,    // 16:10
        R5_4,      // 5:4
    };
    AspectRatio aspect_ratio = AspectRatio::Custom;
    
    // Auto-detection mode
    bool auto_detect = false;  // enable --auto-res mode
    bool auto_res_applied = false;  // mark when auto-resolution has been applied

#ifdef HAVE_WEBGUI
    enum class GuiMode { ImGui, Sdl, Web };
    GuiMode gui_mode = GuiMode::ImGui;
    int web_port = 8080;
#else
    enum class GuiMode { ImGui, Sdl };
    GuiMode gui_mode = GuiMode::ImGui;
#endif

    // CLKIN / CLKOUT (GPSDO support)
    bool clkout = true;    // enable 10 MHz clock output (default: on)
    bool enforce_clkin = false;  // require external CLKIN lock at startup

    // Overlay / OSD customization (ImGui GUI)
    int overlay_font_size = 14;       // default font size (default: 14)
    bool overlay_bold = false;        // use bold font weight (default: off)
    float overlay_color_r = -1.0f;    // R component, -1 = semantic colors
    float overlay_color_g = -1.0f;    // G component, -1 = semantic colors
    float overlay_color_b = -1.0f;    // B component, -1 = semantic colors
    int overlay_margin_x = 8;         // horizontal margin from edge (default: 8)
    int overlay_margin_y = 8;         // vertical margin from edge (default: 8)
    enum class OverlayPos { Top, Bottom };
    OverlayPos overlay_position = OverlayPos::Top;  // top or bottom (default: Top)

    // Overlay section visibility — default true (enabled). Use --no-* to hide.
    bool show_signal = true;          // ring buffer, chroma bar, clipping
    bool show_agc = true;             // manual gain info (LNA/VGA/AMP)
    bool show_clkin = true;           // external clock locked indicator
    bool show_stats = true;           // frames, dropped, clipping, latency

    // Debug mode: run DSP pipeline without GUI, print periodic stats to stdout.
    // Useful for diagnosing source issues (e.g. SoapySDR crashes) without
    // the SDL/ImGui layer.
    bool debug_mode = false;
    int debug_duration_sec = 0;  // 0 = infinite, >0 = auto-exit after N seconds
    double center_hz() const { return video_carrier_hz + offset_hz; }
};

}  // namespace famidec
