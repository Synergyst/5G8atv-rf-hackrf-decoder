#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include "dsp/dc_blocker.hpp"
#include "dsp/fir.hpp"
#include "dsp/fm_detector.hpp"
#include "dsp/frame.hpp"
#include "dsp/nco.hpp"
#include "dsp/ntsc_decoder.hpp"
#include "source/file_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/sample_source.hpp"
#include "ui/sdl_display.hpp"
#include "ui/gui_manager.hpp"
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
        "  --offset HZ           tuning offset above carrier (default 0)\n"
        "  --gain auto|manual    RF gain control (default auto)\n"
        "  --lna N --vga N       gain settings (imply --gain manual)\n"
        "  --no-amp              disable the +14 dB RF preamp (default on)\n"
        "  --mode color|gray     decode mode (default color)\n"
        "  --sat F --hue DEG     color trims\n"
        "  --overscan F          horizontal crop per side 0..0.15 (default 0)\n"
        "  --record PATH         tee raw IQ to .cs8 while decoding\n"
        "  --dump-composite PATH write post-AGC composite as f32\n"
        "  --dump-frames PREFIX  write decoded frames as PPM (headless)\n"
        "  --frames N            number of frames for --dump-frames (default 30)\n"
        "  --spectrum            print PSD and exit (no video)\n"
        "  --gui imgui|sdl       GUI mode: imgui (default, no hotkeys) or sdl (hotkeys)\n"
        "  --resolution WxH      output resolution (default 640x480)\n"
        "  --aspect 4:3|16:9|16:10|5:4|custom  aspect ratio preset\n"
        "  --auto-res            enable auto-detection of signal standard and\n"
        "                        suggest resolution based on detected content\n"
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
        "      arrows tune (50 kHz / 1 MHz), r CRT mode, v record IQ\n");
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
        } else if (a == "--file") { cfg->file_path = next("--file"); cfg->input = Config::Input::File; }
        else if (a == "--loop") cfg->loop = true;
        else if (a == "--rate") cfg->sample_rate = std::atof(next("--rate"));
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
        else if (a == "--record") cfg->record_path = next("--record");
        else if (a == "--dump-composite") cfg->dump_composite_path = next("--dump-composite");
        else if (a == "--dump-frames") { cfg->dump_frames_prefix = next("--dump-frames"); cfg->headless = true; }
        else if (a == "--frames") cfg->dump_frame_count = std::atoi(next("--frames"));
        else if (a == "--spectrum") cfg->spectrum = true;
        else if (a == "--gui") {
            std::string v = next("--gui");
            if (v == "imgui") cfg->gui_mode = Config::GuiMode::ImGui;
            else if (v == "sdl") cfg->gui_mode = Config::GuiMode::Sdl;
            else { std::fprintf(stderr, "gui mode must be imgui|sdl\n"); return false; }
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
            // Parse "X,Y" format
            char *end;
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

void write_ppm(const Frame& f, const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return;
    std::fprintf(fp, "P6\n%d %d\n255\n", f.width, f.height);
    std::vector<uint8_t> rgb(f.rgba.size() * 3);
    size_t o = 0;
    for (uint32_t px : f.rgba) {
        rgb[o++] = static_cast<uint8_t>(px & 0xff);
        rgb[o++] = static_cast<uint8_t>((px >> 8) & 0xff);
        rgb[o++] = static_cast<uint8_t>((px >> 16) & 0xff);
    }
    std::fwrite(rgb.data(), 1, rgb.size(), fp);
    std::fclose(fp);
}

std::atomic<bool> g_running{true};

class Recorder {
public:
    bool start(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        if (fp_) return false;
        fp_ = std::fopen(path.c_str(), "wb");
        if (!fp_) return false;
        path_ = path;
        bytes_ = 0;
        started_ = std::chrono::steady_clock::now();
        return true;
    }

    void write(const uint8_t* data, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fp_) return;
        std::fwrite(data, 1, n, fp_);
        bytes_ += n;
    }

    bool stop(std::string* path, uint64_t* bytes) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fp_) return false;
        std::fclose(fp_);
        fp_ = nullptr;
        if (path) *path = path_;
        if (bytes) *bytes = bytes_;
        return true;
    }

    bool active() {
        std::lock_guard<std::mutex> lk(mu_);
        return fp_ != nullptr;
    }

    float seconds() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fp_) return 0.0f;
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - started_).count();
    }

private:
    std::mutex mu_;
    std::FILE* fp_ = nullptr;
    std::string path_;
    uint64_t bytes_ = 0;
    std::chrono::steady_clock::time_point started_;
};

std::string next_recording_path() {
    for (int i = 1; i < 1000; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "fpvdec_rec_%03d.cs8", i);
        std::FILE* f = std::fopen(name, "rb");
        if (f) {
            std::fclose(f);
            continue;
        }
        return name;
    }
    return "fpvdec_rec_overflow.cs8";
}

double carrier_offset_hz(const NtscDecoder& dec, const Config& cfg) {
    double sgn = cfg.invert ? 1.0 : -1.0;
    double f_tip = sgn * dec.stats().agc_tip_raw.load() * cfg.fm_dev_hz;
    double f_blank = sgn * dec.stats().agc_blank_raw.load() * cfg.fm_dev_hz;
    double f_white = f_blank + 2.5 * (f_blank - f_tip);
    return 0.5 * (f_tip + f_white);
}

void dsp_thread(const Config& cfg, ISampleSource* src, NtscDecoder* dec,
                Recorder* rec, std::atomic<float>* mean_raw) {
    constexpr size_t kBlockBytes = 1 << 16;
    std::vector<uint8_t> raw(kBlockBytes);
    std::vector<std::complex<float>> iq(kBlockBytes / 2);
    std::vector<float> comp(kBlockBytes / 2);

    DcBlocker dcb;
    const bool mix = cfg.offset_hz != 0.0;
    Nco mixer(cfg.offset_hz, cfg.sample_rate);
    FirFilterC chan_lpf(design_lowpass(std::min(8.0e6, cfg.sample_rate * 0.49), cfg.sample_rate, 47));
    FmDetector fm_det(cfg.sample_rate, cfg.fm_dev_hz, cfg.invert);
    const bool use_video_lpf = cfg.video_lpf_hz > 0.0;
    FirFilterF video_lpf(design_lowpass(use_video_lpf ? std::min(cfg.video_lpf_hz, cfg.sample_rate * 0.45) : 1.0, cfg.sample_rate, 63));

    while (g_running.load(std::memory_order_relaxed)) {
        size_t n = src->read(raw.data(), raw.size());
        if (n == 0) break;
        rec->write(raw.data(), n);
        size_t ns = n / 2;
        for (size_t i = 0; i < ns; ++i) {
            std::complex<float> c(static_cast<int8_t>(raw[2 * i]) / 128.0f, static_cast<int8_t>(raw[2 * i + 1]) / 128.0f);
            iq[i] = dcb.process(c);
        }
        if (mix) for (size_t i = 0; i < ns; ++i) iq[i] *= mixer.next();
        chan_lpf.process(iq.data(), iq.data(), ns);
        fm_det.process(iq.data(), comp.data(), ns);
        if (use_video_lpf) video_lpf.process(comp.data(), comp.data(), ns);
        float sum = 0.0f;
        for (size_t i = 0; i < ns; ++i) sum += comp[i];
        float m = mean_raw->load(std::memory_order_relaxed);
        mean_raw->store(0.97f * m + 0.03f * (sum / static_cast<float>(ns)), std::memory_order_relaxed);
        dec->process(comp.data(), ns);
    }
    g_running.store(false, std::memory_order_relaxed);
}

int run_spectrum(const Config& cfg, ISampleSource* src) {
    size_t n_samples = static_cast<size_t>(cfg.sample_rate / 2);
    std::vector<uint8_t> buf(n_samples * 2);
    size_t got = src->read(buf.data(), buf.size());
    if (got < 4096) {
        std::fprintf(stderr, "not enough samples for PSD\n");
        return 1;
    }
    print_psd(reinterpret_cast<const int8_t*>(buf.data()), got / 2, cfg.center_hz(), cfg.sample_rate);
    std::printf("\nexpect: FM energy roughly centered on %.1f MHz, spread over +/-%.1f MHz\n", cfg.video_carrier_hz / 1e6, (cfg.fm_dev_hz + 4.2e6) / 1e6);
    uint64_t clipped = src->clipped_samples();
    if (clipped) std::printf("WARNING: %llu clipped samples - reduce gain\n", static_cast<unsigned long long>(clipped));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, &cfg)) {
        usage();
        return 2;
    }

    std::unique_ptr<ISampleSource> src;
    HackRfSource* hackrf = nullptr;
    if (cfg.input == Config::Input::HackRF) {
        auto h = std::make_unique<HackRfSource>(cfg);
        hackrf = h.get();
        src = std::move(h);
    } else {
        src = std::make_unique<FileSource>(cfg, !cfg.headless && !cfg.spectrum);
    }
    if (!src->start()) {
        std::fprintf(stderr, "failed to start input source%s%s\n", hackrf ? ": " : "", hackrf ? hackrf->error().c_str() : "");
        return 1;
    }
    // CLKIN check: if enforcement is requested, verify external clock is locked
    if (cfg.enforce_clkin && hackrf && !hackrf->check_clkin()) {
        std::fprintf(stderr, "ERROR: --enforce-clkin requires external CLKIN lock. "
                           "Is the GPSDO connected and outputting 10 MHz?\n");
        return 1;
    }
    if (hackrf) {
        std::printf("CLKIN: %s%s\n",
                    hackrf->check_clkin() ? "locked (external clock)" : "unlocked (internal oscillator)",
                    cfg.enforce_clkin && hackrf->check_clkin() ? " [enforced]" : "");
    }
    std::printf("input: %s   video carrier %.3f MHz   center %.3f MHz   %.1f MSPS\n", cfg.input == Config::Input::HackRF ? "HackRF" : cfg.file_path.c_str(), cfg.video_carrier_hz / 1e6, cfg.center_hz() / 1e6, cfg.sample_rate / 1e6);

    if (cfg.spectrum) {
        int rc = run_spectrum(cfg, src.get());
        src->stop();
        return rc;
    }

    Recorder rec;
    if (!cfg.record_path.empty() && !rec.start(cfg.record_path)) {
        std::fprintf(stderr, "cannot open %s\n", cfg.record_path.c_str());
        return 1;
    }

    TripleBuffer tb;
    tb.resize(cfg.frame_width, cfg.frame_height);
    NtscDecoder dec(cfg, tb);
    std::atomic<float> mean_raw{0.0f};
    std::thread dsp(dsp_thread, std::cref(cfg), src.get(), &dec, &rec, &mean_raw);

    int rc = 0;
    if (cfg.headless) {
        uint64_t last_seq = 0;
        int written = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (written < cfg.dump_frame_count && g_running.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
            const Frame* f = tb.acquire();
            if (f && f->seq != last_seq) {
                last_seq = f->seq;
                char path[512];
                std::snprintf(path, sizeof(path), "%s%04d.ppm", cfg.dump_frames_prefix.c_str(), written);
                write_ppm(*f, path);
                ++written;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        std::printf("wrote %d frames; decoded lines=%llu coasted=%llu frames=%llu\n",
                    written,
                    static_cast<unsigned long long>(dec.stats().lines.load()),
                    static_cast<unsigned long long>(dec.stats().lines_coasted.load()),
                    static_cast<unsigned long long>(dec.stats().frames.load()));
        if (dec.stats().line_locked.load())
            std::printf("carrier offset: %+.2f MHz from tuned center (live AFC would re-tune; or use --freq)\n",
                        carrier_offset_hz(dec, cfg) / 1e6);
        if (written == 0) rc = 1;
    } else {
        bool use_imgui = (cfg.gui_mode == Config::GuiMode::ImGui);
        SdlDisplay disp;
        if (!disp.init("fpvdec - FPV ATV decoder", cfg.frame_width, cfg.frame_height)) {
            std::fprintf(stderr, "SDL init failed\n");
            g_running.store(false);
            dsp.join();
            return 1;
        }
        tb.resize(cfg.frame_width, cfg.frame_height);
        disp.rebuild_crt_lut(cfg.frame_width, cfg.frame_height);

        GuiManager gui;
        if (use_imgui) {
            gui.init(disp.win(), disp.renderer(), cfg);
            disp.set_hotkeys_enabled(false);
        }

        Config& mcfg = cfg;
        int shot = 0;
        Frame last_shown;
        bool have_shown = false;
        uint64_t prev_frames = 0;
        auto last_frame_inc = std::chrono::steady_clock::now();
        bool show_help = false;
        bool crt_mode = false;
        bool show_osd = true;
        float fps = 0.0f;
        auto fps_start = std::chrono::steady_clock::now();
        struct FpsAnchor { std::chrono::steady_clock::time_point t; uint64_t frames; };
        FpsAnchor fps_prev{fps_start, 0}, fps_cur{fps_start, 0};
        auto last_afc = fps_start;
        auto lock_changed = fps_start;
        bool was_locked = false;
        std::vector<double> afc_meas;
        auto last_agc = fps_start;
        auto last_clip_seen = fps_start - std::chrono::seconds(10);
        uint64_t last_clip_count = 0;
        std::string channel = fpv_nearest_channel(cfg.video_carrier_hz);
        while (g_running.load(std::memory_order_relaxed)) {
            KeyAction act = disp.poll();
            if (act == KeyAction::Quit) break;

            // Handle hotkey actions first
            if (hackrf) {
                if (act == KeyAction::GainLnaUp || act == KeyAction::GainLnaDown || act == KeyAction::GainVgaUp || act == KeyAction::GainVgaDown)
                    mcfg.gain_auto = false;
                if (act == KeyAction::GainLnaUp) hackrf->set_gains(hackrf->lna() + 8, hackrf->vga());
                if (act == KeyAction::GainLnaDown) hackrf->set_gains(hackrf->lna() - 8, hackrf->vga());
                if (act == KeyAction::GainVgaUp) hackrf->set_gains(hackrf->lna(), hackrf->vga() + 2);
                if (act == KeyAction::GainVgaDown) hackrf->set_gains(hackrf->lna(), hackrf->vga() - 2);
                if (act == KeyAction::ToggleAmp) { mcfg.amp = !mcfg.amp; hackrf->set_amp(mcfg.amp); }
            }
            if (act == KeyAction::ToggleGainMode) mcfg.gain_auto = !mcfg.gain_auto;
            if (act == KeyAction::ToggleHelp) show_help = !show_help;
            if (act == KeyAction::ToggleOsd) show_osd = !show_osd;
            if (act == KeyAction::ToggleRecord) {
                std::string p; uint64_t b;
                if (rec.stop(&p, &b))
                    std::printf("saved %s (%.1f MB) - replay: fpvdec --input file --file %s\n", p.c_str(), b / 1e6, p.c_str());
                else if (rec.start(next_recording_path()))
                    std::printf("recording IQ...\n");
                std::fflush(stdout);
            }
            if (act == KeyAction::ToggleCrt) crt_mode = !crt_mode;
            double tune = 0.0;
            if (act == KeyAction::FreqUp) tune = 50e3;
            if (act == KeyAction::FreqDown) tune = -50e3;
            if (act == KeyAction::FreqUpBig) tune = 1e6;
            if (act == KeyAction::FreqDownBig) tune = -1e6;
            if (tune != 0.0 && hackrf) {
                mcfg.video_carrier_hz += tune;
                hackrf->set_center_freq(mcfg.center_hz());
                channel = fpv_nearest_channel(mcfg.video_carrier_hz);
            }
            if (act == KeyAction::ToggleColor)
                mcfg.mode = (mcfg.mode == Config::Mode::Color) ? Config::Mode::Gray : Config::Mode::Color;

            // Acquire the latest frame
            const Frame* f = tb.acquire();
            if (f) { last_shown = *f; have_shown = true; }
            if (act == KeyAction::Screenshot && have_shown) {
                char path[64]; std::snprintf(path, sizeof(path), "fpvdec_%03d.bmp", shot++);
                disp.screenshot(last_shown, path);
                std::printf("saved %s\n", path);
            }

            // Build stats from decoder and source
            OsdStats st;
            st.line_locked = dec.stats().line_locked.load();
            st.burst_amp = dec.stats().burst_amp.load();
            st.ring_fill = src->ring_fill();
            st.dropped = src->dropped_bytes();
            st.clipped = src->clipped_samples();
            st.frames = dec.stats().frames.load();
            st.lines = dec.stats().lines.load();
            if (hackrf) { st.lna = hackrf->lna(); st.vga = hackrf->vga(); }
            st.amp = mcfg.amp;
            st.gain_auto = mcfg.gain_auto;
            st.clkin_locked = hackrf ? hackrf->check_clkin() : false;
            
            // Auto-resolution: apply detected resolution after lock is acquired
            if (mcfg.auto_detect && !mcfg.auto_res_applied && 
                dec.stats().auto_detect_ready.load(std::memory_order_acquire)) {
                double chroma_hz = dec.stats().detected_chroma_hz.load(std::memory_order_acquire);
                int active_lines = dec.stats().detected_active_lines.load(std::memory_order_acquire);
                int horiz_detail = dec.stats().detected_horiz_detail.load(std::memory_order_acquire);
                int line_rate_mhz = dec.stats().detected_line_rate.load(std::memory_order_acquire);
                
                // Determine standard from chroma frequency
                bool is_pal = (chroma_hz > 4.0e6);  // PAL ~4.43 MHz vs NTSC ~3.58 MHz
                
                // If active_lines wasn't counted (grayscale or no burst), use standard values
                if (active_lines <= 0) {
                    active_lines = is_pal ? 288 : 240;  // PAL vs NTSC standard
                }
                
                // Apply aspect ratio preset to calculate target dimensions
                int target_width = mcfg.frame_width;
                int target_height = mcfg.frame_height;
                
                if (mcfg.aspect_ratio != Config::AspectRatio::Custom && 
                    mcfg.aspect_ratio != Config::AspectRatio::R4_3 &&
                    mcfg.aspect_ratio != Config::AspectRatio::R16_9 &&
                    mcfg.aspect_ratio != Config::AspectRatio::R16_10 &&
                    mcfg.aspect_ratio != Config::AspectRatio::R5_4) {
                    // Custom resolution — use user-specified
                } else if (mcfg.aspect_ratio == Config::AspectRatio::R16_9) {
                    // 16:9 widescreen
                    target_width = static_cast<int>(target_height * 16.0 / 9.0 + 0.5);
                } else if (mcfg.aspect_ratio == Config::AspectRatio::R16_10) {
                    // 16:10
                    target_width = static_cast<int>(target_height * 16.0 / 10.0 + 0.5);
                } else if (mcfg.aspect_ratio == Config::AspectRatio::R5_4) {
                    // 5:4
                    target_width = static_cast<int>(target_height * 5.0 / 4.0 + 0.5);
                } else if (mcfg.aspect_ratio == Config::AspectRatio::R4_3) {
                    // 4:3 standard
                    target_width = static_cast<int>(target_height * 4.0 / 3.0 + 0.5);
                }
                
                // If user specified --resolution, respect it
                if (mcfg.aspect_ratio == Config::AspectRatio::Custom) {
                    target_width = mcfg.frame_width;
                    target_height = mcfg.frame_height;
                }
                
                // Apply the new resolution
                mcfg.frame_width = target_width;
                mcfg.frame_height = target_height;
                mcfg.auto_res_applied = true;
                
                // Resize the pipeline
                tb.resize(mcfg.frame_width, mcfg.frame_height);
                disp.rebuild_crt_lut(mcfg.frame_width, mcfg.frame_height);
                
                // Report what we detected
                std::printf("AUTO-RES: detected %s (chroma=%.2f MHz, %d active lines, "
                           "horiz_detail=%d, line_rate=%.3f kHz)\n",
                           is_pal ? "PAL" : "NTSC",
                           chroma_hz / 1e6, active_lines,
                           horiz_detail, line_rate_mhz / 1000.0);
                std::printf("  -> applying resolution: %dx%d\n", 
                           mcfg.frame_width, mcfg.frame_height);
                std::fflush(stdout);
            }
            
            auto now = std::chrono::steady_clock::now();
            if (st.clipped > last_clip_count) { last_clip_count = st.clipped; last_clip_seen = now; }
            st.clipping = (now - last_clip_seen) < std::chrono::seconds(1);
            if (mcfg.gain_auto && hackrf && now - last_agc >= std::chrono::milliseconds(500)) {
                last_agc = now;
                int peak = hackrf->take_peak();
                int lna = hackrf->lna(), vga = hackrf->vga();
                if (peak >= 124) {
                    int down = peak >= 127 ? 12 : 6;
                    if (vga > 0) hackrf->set_gains(lna, vga - down);
                    else if (lna > 0) hackrf->set_gains(lna - 8, vga);
                } else if (peak > 0 && peak < 64) {
                    if (vga < 62) hackrf->set_gains(lna, vga + 2);
                    else if (lna < 40) hackrf->set_gains(lna + 8, vga);
                }
                if (hackrf->lna() != lna || hackrf->vga() != vga) {
                    std::printf("AGC: peak %d -> LNA%d VGA%d\n", peak, hackrf->lna(), hackrf->vga());
                    std::fflush(stdout);
                }
            }
            if (st.frames > prev_frames) { prev_frames = st.frames; last_frame_inc = now; }
            st.vsync_locked = (now - last_frame_inc) < std::chrono::milliseconds(500);
            if (!st.vsync_locked) { fps = 0.0f; fps_prev = fps_cur = {now, st.frames}; }
            else {
                if (now - fps_cur.t >= std::chrono::seconds(8)) { fps_prev = fps_cur; fps_cur = {now, st.frames}; }
                double win = std::chrono::duration<double>(now - fps_prev.t).count();
                if (win >= 1.0) fps = static_cast<float>(static_cast<double>(st.frames - fps_prev.frames) / win);
            }
            st.fps = fps;
            st.show_osd = show_osd;
            // AFC
            {
                bool locked = st.line_locked && st.vsync_locked;
                if (locked != was_locked) { was_locked = locked; lock_changed = now; afc_meas.clear(); }
                if (cfg.afc && hackrf && now - last_afc >= std::chrono::milliseconds(500) && now - lock_changed >= std::chrono::seconds(2)) {
                    last_afc = now;
                    afc_meas.push_back(locked ? carrier_offset_hz(dec, cfg) :
                        (cfg.invert ? 1.0 : -1.0) * static_cast<double>(mean_raw.load()) * cfg.fm_dev_hz);
                    if (afc_meas.size() >= 3) {
                        std::sort(afc_meas.begin(), afc_meas.end());
                        double med = afc_meas[1], spread = afc_meas[2] - afc_meas[0];
                        bool same_sign = (afc_meas[0] > 0.0) == (afc_meas[2] > 0.0);
                        double thresh = locked ? 150e3 : 300e3;
                        double clampv = locked ? 1e6 : 2e6;
                        double max_spread = locked ? 200e3 : 600e3;
                        afc_meas.clear();
                        if (std::abs(med) > thresh && same_sign && spread < max_spread) {
                            double step = std::clamp(med, -clampv, clampv);
                            mcfg.video_carrier_hz += step;
                            hackrf->set_center_freq(mcfg.center_hz());
                            dec.shift_levels(static_cast<float>((cfg.invert ? -step : step) / cfg.fm_dev_hz));
                            channel = fpv_nearest_channel(mcfg.video_carrier_hz);
                            std::printf("AFC%s: %+0.2f MHz -> %.2f MHz\n", locked ? "" : " (coarse)", step / 1e6, mcfg.video_carrier_hz / 1e6);
                            std::fflush(stdout);
                        }
                    }
                }
            }
            st.show_help = show_help;
            st.crt = crt_mode;
            st.recording = rec.active();
            st.rec_seconds = rec.seconds();
            if (st.vsync_locked) {
                uint64_t dsp_samples = dec.stats().samples_in.load();
                uint64_t vsync_pos = dec.stats().frame_sample_pos.load();
                if (dsp_samples > vsync_pos)
                    st.video_latency_ms = static_cast<float>(
                        (static_cast<double>(dsp_samples - vsync_pos) +
                         static_cast<double>(src->buffered_bytes()) / 2.0) /
                            cfg.sample_rate * 1000.0 + 13.0);
            }
            st.freq_mhz = cfg.video_carrier_hz / 1e6;
            st.channel = channel;

            // Render
            float gui_screenshot = 0.0f;
            if (use_imgui) {
                disp.render_video_only(f);
                gui_screenshot = gui.render(st);
                SDL_RenderPresent(disp.renderer());
                if (gui_screenshot > 0.0f && have_shown) {
                    char path[64]; std::snprintf(path, sizeof(path), "fpvdec_%03d.bmp", shot++);
                    disp.screenshot(last_shown, path);
                    std::printf("saved %s\n", path);
                }
            } else {
                disp.render(f, st);
            }
        }
        if (use_imgui) gui.shutdown();
    }
    g_running.store(false, std::memory_order_relaxed);
    src->stop();
    dsp.join();
    std::string rec_path; uint64_t rec_bytes = 0;
    if (rec.stop(&rec_path, &rec_bytes))
        std::printf("saved %s (%.1f MB) - replay: fpvdec --input file --file %s\n", rec_path.c_str(), rec_bytes / 1e6, rec_path.c_str());
    return rc;
}
