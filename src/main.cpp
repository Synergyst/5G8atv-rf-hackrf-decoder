#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "config_store.hpp"
#include "runtime_control.hpp"
#include "runtime_lifecycle.hpp"
#include "runtime_session.hpp"
#include "runtime_coordinator.hpp"
#include "runtime_modes.hpp"
#include "runtime_recording.hpp"
#include "dsp/dc_blocker.hpp"
#include "dsp/fir.hpp"
#include "dsp/fm_detector.hpp"
#include "dsp/frame.hpp"
#include "dsp/nco.hpp"
#include "dsp/ntsc_decoder.hpp"
#include "filter/denoiser.hpp"
#include "filter/filter.hpp"
#include "filter/temporal.hpp"
#include "filter/temporal_median.hpp"
#include "source/file_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/sample_source.hpp"
#ifdef HAVE_UHD
#include "source/uhd_source.hpp"
#endif
#ifdef HAVE_SOAPYSDR
#include "source/soapy_source.hpp"
#endif
#include "ui/sdl_display.hpp"
#include "ui/gui_manager.hpp"
#ifdef HAVE_WEBGUI
#include "ui/web_display.hpp"
#endif
#include "util/fpv_channels.hpp"
#include "util/spectrum.hpp"

using namespace famidec;


namespace {

void usage() {
    std::printf(
        "fpvdec - 5.8 GHz analog FPV (FM-ATV, NTSC) HackRF One decoder\n\n"
        "usage: fpvdec [options]\n"
        "  --channel NAME        FPV channel preset, band A/B/E/F/R + 1-8\n"
        "                        e.g. F4, R1 (default F4 = 5800 MHz)\n"
        "  --freq HZ             explicit carrier frequency\n"
        "  --dev HZ              FM peak deviation (default 5e6)\n"
        "  --invert              flip discriminator polarity (non-standard VTX)\n"
        "  --no-afc              disable automatic centering on the VTX\n"
        "  --lpf HZ              optional post-detector video LPF, e.g. 4.2e6\n"
        "                        for the NTSC video bandwidth (default off)\n"
        "  --input hackrf|file   input source (default hackrf)\n"
        "  --file PATH           .cs8 recording for --input file\n"
        "  --loop                loop file playback\n"
        "  --rate HZ             sample rate (default 10e6). Lower = less CPU:\n"
        "                        8e6 keeps coarse color, <=7e6 grayscale,\n"
        "                        ~6e6 practical minimum (use whole MHz)\n"
        "  --bits 8|16           IQ sample width for UHD/SoapySDR (default 8)\n"
        "  --config PATH         persisted configuration file (default fpvdec.json)\n"
        "  --offset HZ           tuning offset above carrier (default 0)\n"
        "  --gain auto|manual    RF gain control (default auto)\n"
        "  --lna N --vga N       gain settings (imply --gain manual)\n"
        "  --no-amp              disable the +14 dB RF preamp (default on)\n"
        "  --mode color|gray     decode mode (default color)\n"
        "  --sat F --hue DEG     color trims\n"
        "  --denoise F           spatial denoise 0.0..1.0 (default 0.0)\n"
        "  --denoise-temporal F  temporal IIR denoise 0.0..1.0 (default 0.0)\n"
        "  --denoise-temporal-median N  N-frame temporal median, N=3..9 (default 0=off)\n"
        "  --denoise-temporal-median-strength S  blend strength 0.0..1.0 (default 1.0)\n"
        "  --overscan F          horizontal crop per side 0..0.15 (default 0)\n"
        "  --record PATH         tee raw IQ to .cs8 while decoding\n"
        "  --dump-composite PATH write post-AGC composite as f32\n"
        "  --dump-frames PREFIX  write decoded frames as PPM (headless)\n"
        "  --frames N            number of frames for --dump-frames (default 30)\n"
        "  --spectrum            print PSD and exit (no video)\n"
#ifdef HAVE_WEBGUI
        "  --gui imgui|sdl|web   GUI mode: imgui (default), sdl, or web (headless server)\n"
        "  --web-port N          web server port (default: 8080)\n"
#else
        "  --gui imgui|sdl       GUI mode: imgui (default, no hotkeys) or sdl (hotkeys)\n"
#endif
        "  --resolution WxH      output resolution (default 640x480)\n"
        "  --aspect 4:3|16:9|16:10|5:4|custom  aspect ratio preset\n"
#ifdef HAVE_SOAPYSDR
        "  --source hackrf|file|soapysdr|uhd  input source (default: hackrf)\n"
        "  --device ARGS         SoapySDR device args (e.g. 'driver=uhd')\n"
#endif
#ifdef HAVE_UHD
        "  --source uhd          native UHD input source\n"
        "  --uhd-device ARGS    native UHD device args (e.g. 'addr=192.168.10.2')\n"
        "  --uhd-gain DB        native UHD aggregate RX gain (default: 45)\n"
        "  --antenna NAME       native UHD RX antenna name\n"
#endif
        "  --enforce-clkin       require external CLKIN lock at startup\n"
        "  --no-clkout           disable CLKOUT (default: 10 MHz output on)\n"
        "\nImGui overlay options:\n"
        "  --overlay-font N      font size (default 14)\n"
        "  --overlay-color R,G,B RGB color 0.0..1.0 (default: semantic colors)\n"
        "  --overlay-margin X,Y  X and Y margin in pixels (default 8)\n"
        "  --overlay-top         position overlay at top (default)\n"
        "  --overlay-bottom      position overlay at bottom\n"
        "  --no-signal           hide signal quality bars (ring/chroma)\n"
        "  --no-agc              hide manual AGC gain info\n"
        "  --no-clkin            hide CLKIN status\n"
        "  --no-stats            hide frame/dropped/latency stats\n"
        "\nkeys (SDL mode only): q/ESC quit, a gain auto/manual, l/L LNA, g/G VGA, b RF amp,\n"
        "      c color, o OSD on/off, s screenshot, h help,\n"
        "      arrows tune (50 kHz / 1 MHz), r CRT mode, v record IQ\n"
        "\nDebug mode (no GUI):\n"
        "  --debug               run DSP pipeline without SDL/ImGui,\n"
        "                        print periodic stats to stdout.\n"
        "                        Ctrl+C or --debug-duration N to stop.\n"
        "  --debug-duration N    auto-stop after N seconds (0=forever)\n");
}

bool parse_args(int argc, char** argv, Config* cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--channel") {
            std::string v = next("--channel");
            if (!fpv_channel_freq(v, &cfg->video_carrier_hz)) {
                std::fprintf(stderr,
                             "bad channel %s (band A/B/E/F/R + 1-8, e.g. F4)\n",
                             v.c_str());
                return false;
            }
        } else if (a == "--freq") cfg->video_carrier_hz = std::atof(next("--freq"));
        else if (a == "--input") {
            std::string v = next("--input");
            if (v == "hackrf") cfg->input = Config::Input::HackRF;
            else if (v == "file") cfg->input = Config::Input::File;
            else return false;
        } else if (a == "--source") {
            std::string v = next("--source");
            if (v == "hackrf") {
                cfg->input = Config::Input::HackRF;
            } else if (v == "file") {
                cfg->input = Config::Input::File;
            } else if (v == "soapysdr") {
#ifdef HAVE_SOAPYSDR
                cfg->input = Config::Input::SoapySDR;
#else
                std::fprintf(stderr, "error: SoapySDR support not compiled in\n");
                return false;
#endif
            } else if (v == "uhd") {
#ifdef HAVE_UHD
                cfg->input = Config::Input::UHD;
#else
                std::fprintf(stderr, "error: native UHD support not compiled in\n");
                return false;
#endif
            } else {
                std::fprintf(stderr, "source must be hackrf|file|soapysdr|uhd\n");
                return false;
            }
        } else if (a == "--file") { cfg->file_path = next("--file"); cfg->input = Config::Input::File; }
        else if (a == "--config") cfg->config_path = next("--config");
#ifdef HAVE_SOAPYSDR
        else if (a == "--device") cfg->soapysdr_device_args = next("--device");
#endif
#ifdef HAVE_UHD
        else if (a == "--uhd-device") cfg->uhd_device_args = next("--uhd-device");
        else if (a == "--uhd-gain") cfg->uhd_gain_db = std::atof(next("--uhd-gain"));
        else if (a == "--antenna") cfg->uhd_antenna = next("--antenna");
#endif
        else if (a == "--loop") cfg->loop = true;
        else if (a == "--rate") cfg->sample_rate = std::atof(next("--rate"));
        else if (a == "--bits") {
            cfg->sample_bits = std::atoi(next("--bits"));
            if (cfg->sample_bits != 8 && cfg->sample_bits != 16) {
                std::fprintf(stderr, "--bits must be 8 or 16\n"); return false;
            }
        }
        else if (a == "--offset") cfg->offset_hz = std::atof(next("--offset"));
        else if (a == "--lna") { cfg->lna_gain = std::atoi(next("--lna")); cfg->gain_auto = false; }
        else if (a == "--vga") { cfg->vga_gain = std::atoi(next("--vga")); cfg->gain_auto = false; }
        else if (a == "--amp") cfg->amp = true;
        else if (a == "--no-amp") cfg->amp = false;
        else if (a == "--gain") {
            std::string v = next("--gain");
            if (v == "auto") cfg->gain_auto = true;
            else if (v == "manual") cfg->gain_auto = false;
            else { std::fprintf(stderr, "gain must be auto|manual\n"); return false; }
        }
        else if (a == "--mode") {
            std::string v = next("--mode");
            cfg->mode = (v == "gray") ? Config::Mode::Gray : Config::Mode::Color;
        } else if (a == "--dev") cfg->fm_dev_hz = std::atof(next("--dev"));
        else if (a == "--invert") cfg->invert = true;
        else if (a == "--no-afc") cfg->afc = false;
        else if (a == "--lpf") cfg->video_lpf_hz = std::atof(next("--lpf"));
        else if (a == "--overscan") cfg->overscan = std::atof(next("--overscan"));
        else if (a == "--sat") cfg->saturation = std::atof(next("--sat"));
        else if (a == "--hue") cfg->hue_deg = std::atof(next("--hue"));
        else if (a == "--denoise") {
            cfg->denoise = std::atof(next("--denoise"));
            cfg->denoise = std::clamp(cfg->denoise, 0.0f, 1.0f);
        }
        else if (a == "--denoise-temporal") {
            cfg->denoise_temporal = std::atof(next("--denoise-temporal"));
            cfg->denoise_temporal = std::clamp(cfg->denoise_temporal, 0.0f, 1.0f);
        }
        else if (a == "--denoise-temporal-median") {
            cfg->denoise_temporal_median = std::atoi(next("--denoise-temporal-median"));
            cfg->denoise_temporal_median = std::clamp(cfg->denoise_temporal_median, 0, 9);
            if (cfg->denoise_temporal_median % 2 == 0) cfg->denoise_temporal_median++;  // Ensure odd
        }
        else if (a == "--denoise-temporal-median-strength") {
            cfg->denoise_temporal_median_strength = std::atof(next("--denoise-temporal-median-strength"));
            cfg->denoise_temporal_median_strength = std::clamp(cfg->denoise_temporal_median_strength, 0.0f, 1.0f);
        }
        else if (a == "--record") cfg->record_path = next("--record");
        else if (a == "--dump-composite") cfg->dump_composite_path = next("--dump-composite");
        else if (a == "--dump-frames") { cfg->dump_frames_prefix = next("--dump-frames"); cfg->headless = true; }
        else if (a == "--frames") cfg->dump_frame_count = std::atoi(next("--frames"));
        else if (a == "--spectrum") cfg->spectrum = true;
        else if (a == "--gui") {
            std::string v = next("--gui");
            if (v == "imgui") cfg->gui_mode = Config::GuiMode::ImGui;
            else if (v == "sdl") cfg->gui_mode = Config::GuiMode::Sdl;
#ifdef HAVE_WEBGUI
            else if (v == "web") cfg->gui_mode = Config::GuiMode::Web;
            else { std::fprintf(stderr, "gui mode must be imgui|sdl|web\n"); return false; }
#else
            else { std::fprintf(stderr, "gui mode must be imgui|sdl\n"); return false; }
#endif
        } else if (a == "--web-port") {
#ifdef HAVE_WEBGUI
            cfg->web_port = std::atoi(next("--web-port"));
            if (cfg->web_port <= 0) cfg->web_port = 8080;
#else
            next("--web-port");  // consume argument, warn
            std::fprintf(stderr, "warning: --web-port ignored (Web GUI not compiled in)\n");
#endif
        } else if (a == "--enforce-clkin") cfg->enforce_clkin = true;
        else if (a == "--no-clkout") cfg->clkout = false;
        else if (a == "--overlay-font") cfg->overlay_font_size = std::atoi(next("--overlay-font"));
        else if (a == "--overlay-color") {
            std::string v = next("--overlay-color");
            // Parse "R,G,B" format
            char *end;
            float r = std::strtof(v.c_str(), &end);
            if (*end != ',') {
                std::fprintf(stderr, "--overlay-color: expected R,G,B format\n");
                return false;
            }
            float g = std::strtof(end + 1, &end);
            if (*end != ',') {
                std::fprintf(stderr, "--overlay-color: expected R,G,B format\n");
                return false;
            }
            float b = std::strtof(end + 1, &end);
            if (*end != '\0') {
                std::fprintf(stderr, "--overlay-color: expected R,G,B format\n");
                return false;
            }
            cfg->overlay_color_r = r;
            cfg->overlay_color_g = g;
            cfg->overlay_color_b = b;
        }
        else if (a == "--overlay-margin") {
            std::string v = next("--overlay-margin");
            cfg->overlay_margin_x = std::atoi(v.c_str());
            auto comma = v.find(',');
            if (comma != std::string::npos) {
                cfg->overlay_margin_y = std::atoi(v.c_str() + comma + 1);
            }
        }
        else if (a == "--overlay-top") cfg->overlay_position = Config::OverlayPos::Top;
        else if (a == "--overlay-bottom") cfg->overlay_position = Config::OverlayPos::Bottom;
        else if (a == "--no-signal") cfg->show_signal = false;
        else if (a == "--no-agc") cfg->show_agc = false;
        else if (a == "--no-clkin") cfg->show_clkin = false;
        else if (a == "--no-stats") cfg->show_stats = false;
        else if (a == "--debug") cfg->debug_mode = true;
        else if (a == "--debug-duration") {
            cfg->debug_duration_sec = std::atoi(next("--debug-duration"));
            if (cfg->debug_duration_sec < 0) cfg->debug_duration_sec = 0;
        }
        else if (a == "--auto-res") cfg->auto_detect = true;
        else if (a == "--resolution") {
            std::string v = next("--resolution");
            if (v.find('x') == std::string::npos) {
                std::fprintf(stderr, "--resolution: expected format WxH (e.g. 640x480)\n");
                return false;
            }
            char *end;
            cfg->frame_width = std::strtol(v.c_str(), &end, 10);
            if (*end != 'x') {
                std::fprintf(stderr, "--resolution: expected format WxH\n");
                return false;
            }
            cfg->frame_height = std::strtol(end + 1, &end, 10);
            if (*end != '\0') {
                std::fprintf(stderr, "--resolution: expected format WxH\n");
                return false;
            }
            if (cfg->frame_width <= 0 || cfg->frame_height <= 0) {
                std::fprintf(stderr, "--resolution: dimensions must be positive\n");
                return false;
            }
        } else if (a == "--aspect") {
            std::string v = next("--aspect");
            if (v == "4:3") cfg->aspect_ratio = Config::AspectRatio::R4_3;
            else if (v == "16:9") cfg->aspect_ratio = Config::AspectRatio::R16_9;
            else if (v == "16:10") cfg->aspect_ratio = Config::AspectRatio::R16_10;
            else if (v == "5:4") cfg->aspect_ratio = Config::AspectRatio::R5_4;
            else if (v == "custom") cfg->aspect_ratio = Config::AspectRatio::Custom;
            else {
                std::fprintf(stderr, "--aspect: must be 4:3, 16:9, 16:10, 5:4, or custom\n");
                return false;
            }
        } else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    if (cfg->input == Config::Input::File && cfg->file_path.empty()) {
        std::fprintf(stderr, "--input file requires --file PATH\n");
        return false;
    }
    if (cfg->headless && cfg->dump_frame_count <= 0) cfg->dump_frame_count = 30;
    return true;
}


} // namespace

#include "application_runner.hpp"

int main(int argc, char** argv) {
    Config defaults;
    Config cfg = defaults;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--config") cfg.config_path = argv[i + 1];
    }
    load_config_file(cfg, cfg.config_path);
    if (!parse_args(argc, argv, &cfg)) {
        usage();
        return 2;
    }
    save_config_file(cfg, cfg.config_path);

    Config startup_baseline = defaults;
    startup_baseline.config_path = cfg.config_path;
    if (!parse_args(argc, argv, &startup_baseline)) return 2;

    for (;;) {
        g_running.store(true, std::memory_order_relaxed);
        int rc = run_application(cfg, startup_baseline);
        if (rc != 75) return rc;

        // The Web UI has already persisted cfg. Reuse this live configuration
        // and rebuild every source/DSP/display object from scratch.
        cfg.auto_res_applied = false;
        std::printf("Application reinitialized from persisted runtime configuration.\n");
        std::fflush(stdout);
    }
}
