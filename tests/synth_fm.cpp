// Golden test for the FPV FM path: synthesize an FM-ATV NTSC color-bar
// signal as int8 IQ (composite frequency-modulates the carrier, sync tip at
// the lowest frequency), run it through the same DSP chain as the live FM
// pipeline, and assert the decoded RGB values match the transmitted bars.
//
// Usage:
//   synth_fm                           // run golden test (30 fields, default params)
//   synth_fm --help                    // show help
//   synth_fm --fields N                // use N fields instead of default 30
//   synth_fm --repeat N                // run test N times (stress test, default 1)
//   synth_fm --rate HZ                 // sample rate (default 10e6)
//   synth_fm --dev HZ                  // FM deviation (default 2.5e6)
//   synth_fm --check-frames            // check all decoded frames, not just last
//   synth_fm output.cs8                // write synthetic IQ to file and exit

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "config.hpp"
#include "dsp/dc_blocker.hpp"
#include "dsp/fir.hpp"
#include "dsp/fm_detector.hpp"
#include "dsp/frame.hpp"
#include "dsp/ntsc_decoder.hpp"

using namespace famidec;

namespace {

// Default pipeline rate; deviation matches a real 5.8 GHz VTX (~2.5 MHz
// peak — +-5 MHz would sit at the 10 MSPS Nyquist edge where the phase
// step +-pi is ambiguous).
constexpr double kDefaultFs = 10e6;
constexpr double kDefaultDev = 2.5e6;
constexpr double kFsc = 315e6 / 88.0;
constexpr double kLineUs = 1e6 / 15734.264;  // 63.555 us
constexpr int kLinesPerField = 262;
constexpr int kVsyncLines = 3;
constexpr int kPostVsyncBlank = 13;  // matches NtscDecoder::kActiveStartLine

struct Bar {
    float r, g, b;  // 0..1
};
const Bar kBars[7] = {
    {0.75f, 0.75f, 0.75f}, {0.75f, 0.75f, 0.0f}, {0.0f, 0.75f, 0.75f},
    {0.0f, 0.75f, 0.0f},   {0.75f, 0.0f, 0.75f}, {0.75f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.75f},
};

void yuv_of(const Bar& bar, float* y, float* u, float* v) {
    *y = 0.299f * bar.r + 0.587f * bar.g + 0.114f * bar.b;
    *u = 0.492f * (bar.b - *y);
    *v = 0.877f * (bar.r - *y);
}

// Composite IRE value at a given position within a field.
float composite_ire(int line, double us, double theta) {
    bool vsync_line = line < kVsyncLines;
    if (vsync_line) {
        // broad pulse: asserted except a 4.7 us serration at end of line
        return (us > kLineUs - 4.7) ? 0.0f : -40.0f;
    }
    if (us < 4.7) return -40.0f;                       // hsync
    if (us >= 5.3 && us < 7.8)                          // burst on back porch
        return -20.0f * static_cast<float>(std::sin(theta));
    if (us < 9.4 || us >= 62.0) return 0.0f;            // porches
    int active_line = line - kVsyncLines - kPostVsyncBlank;
    if (active_line < 0 || active_line >= 240) return 0.0f;  // blank line
    double frac = (us - 9.4) / 52.6;
    int bar_idx = std::min(6, static_cast<int>(frac * 7.0));
    float y, u, v;
    yuv_of(kBars[bar_idx], &y, &u, &v);
    float chroma = u * static_cast<float>(std::sin(theta)) +
                   v * static_cast<float>(std::cos(theta));
    return (y + chroma) * 100.0f;
}

void print_usage() {
    std::printf(
        "synth_fm - Generate synthetic FM-ATV NTSC color-bar IQ or run golden test\n"
        "\n"
        "Usage:\n"
        "  synth_fm [options] [output.cs8]\n"
        "\n"
        "Arguments:\n"
        "  output.cs8              Write synthetic IQ to file and exit (if provided)\n"
        "\n"
        "Options:\n"
        "  --help, -h              Show this help message\n"
        "  --fields N              Number of NTSC fields to generate (default: 30)\n"
        "  --repeat N              Run the test N times (default: 1, use for stress tests)\n"
        "  --rate HZ               Sample rate in Hz (default: 10e6)\n"
        "  --dev HZ                FM peak deviation in Hz (default: 2.5e6)\n"
        "  --check-frames          Check ALL decoded frames, not just the last one\n"
        "  --width W               Output frame width in pixels (default: 640)\n"
        "  --height H              Output frame height in pixels (default: 480)\n"
        "\n"
        "Examples:\n"
        "  synth_fm                           # golden test, 30 fields, default params\n"
        "  synth_fm --fields 100              # generate 100 fields for long-run testing\n"
        "  synth_fm --fields 100 --repeat 5   # stress test: 100 fields, 5 iterations\n"
        "  synth_fm --check-frames            # check every decoded frame for errors\n"
        "  synth_fm bars.cs8                  # write IQ file for later replay\n"
        "  synth_fm --rate 8e6 --dev 5e6      # test with different rate/deviation\n"
        "\n"
        "Exit codes:\n"
        "  0  All checks passed\n"
        "  1  Test failure (decode error, color mismatch, or fewer than 10 frames)\n"
        "  2  Usage error (bad arguments)\n"
    );
}

// Run a single test iteration with the given number of fields.
// Returns 0 on success, 1 on failure.
int run_test(int fields, double sample_rate, double dev_hz,
             bool check_all_frames, int width, int height) {
    const size_t total = static_cast<size_t>(fields * kLinesPerField *
                                             kLineUs * sample_rate / 1e6);
    
    // FM modulate: sync tip (-40 IRE) -> -dev, white (+100 IRE) -> +dev.
    std::vector<uint8_t> iq(total * 2);
    const double omega_sc = 2.0 * M_PI * kFsc / sample_rate;
    const double samples_per_line = sample_rate * kLineUs / 1e6;
    const double samples_per_field = samples_per_line * kLinesPerField;
    double phase = 0.0;
    for (size_t n = 0; n < total; ++n) {
        double in_field = std::fmod(static_cast<double>(n), samples_per_field);
        int line = static_cast<int>(in_field / samples_per_line);
        double us = std::fmod(in_field, samples_per_line) / sample_rate * 1e6;
        double theta = std::fmod(omega_sc * static_cast<double>(n), 2.0 * M_PI);
        float ire = composite_ire(line, us, theta);
        double f = dev_hz * ((ire + 40.0) / 70.0 - 1.0);
        phase = std::fmod(phase + 2.0 * M_PI * f / sample_rate, 2.0 * M_PI);
        auto clip8 = [](double x) {
            return static_cast<int8_t>(std::lround(std::fmax(-127.0, std::fmin(127.0, x))));
        };
        iq[2 * n] = static_cast<uint8_t>(clip8(100.0 * std::cos(phase)));
        iq[2 * n + 1] = static_cast<uint8_t>(clip8(100.0 * std::sin(phase)));
    }

    // Same chain as the live FM pipeline (default: no post-detector LPF).
    TripleBuffer tb;
    tb.resize(width, height);
    Config cfg;
    cfg.sample_rate = sample_rate;
    cfg.fm_dev_hz = dev_hz;
    cfg.offset_hz = 0.0;
    cfg.mode = Config::Mode::Color;
    cfg.frame_width = width;
    cfg.frame_height = height;
    NtscDecoder dec(cfg, tb);
    DcBlocker dcb;
    FirFilterC chan_lpf(design_lowpass(std::min(8.0e6, sample_rate * 0.49), sample_rate, 47));
    FmDetector fm_det(sample_rate, cfg.fm_dev_hz, cfg.invert);

    constexpr size_t kBlock = 32768;
    std::vector<std::complex<float>> cbuf(kBlock);
    std::vector<float> comp(kBlock);
    for (size_t off = 0; off < total; off += kBlock) {
        size_t ns = std::min(kBlock, total - off);
        for (size_t i = 0; i < ns; ++i) {
            std::complex<float> c(
                static_cast<int8_t>(iq[2 * (off + i)]) / 128.0f,
                static_cast<int8_t>(iq[2 * (off + i) + 1]) / 128.0f);
            cbuf[i] = dcb.process(c);
        }
        chan_lpf.process(cbuf.data(), cbuf.data(), ns);
        fm_det.process(cbuf.data(), comp.data(), ns);
        dec.process(comp.data(), ns);
    }

    uint64_t frames = dec.stats().frames.load();
    std::printf("  frames=%llu lines=%llu coasted=%llu\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(dec.stats().lines.load()),
                static_cast<unsigned long long>(dec.stats().lines_coasted.load()));
    if (frames < 10) {
        std::printf("  FAIL: fewer than 10 frames decoded\n");
        return 1;
    }

    // Collect all unique frames (acquire advances the ring buffer, so we loop)
    std::vector<const Frame*> all_frames;
    uint64_t last_seq = 0;
    int max_iterations = static_cast<int>(frames) + 5;  // safety limit
    for (int i = 0; i < max_iterations; ++i) {
        const Frame* f = tb.acquire();
        if (!f) break;
        if (f->seq == last_seq) break;  // no new frame
        last_seq = f->seq;
        all_frames.push_back(f);
    }

    if (all_frames.empty()) {
        std::printf("  FAIL: no frames available from TripleBuffer\n");
        return 1;
    }

    int failures = 0;
    int checked = 0;
    for (const Frame* f : all_frames) {
        for (int b = 0; b < 7; ++b) {
            int px = static_cast<int>((b + 0.5) / 7.0 * f->width);
            int py = f->height / 2;
            if (static_cast<size_t>(py) >= static_cast<size_t>(f->height)) continue;
            if (static_cast<size_t>(px) >= static_cast<size_t>(f->width)) continue;
            uint32_t p = f->rgba[static_cast<size_t>(py) * f->width + px];
            int r = p & 0xff, g = (p >> 8) & 0xff, bl = (p >> 16) & 0xff;
            int er = static_cast<int>(kBars[b].r * 255.0f + 0.5f);
            int eg = static_cast<int>(kBars[b].g * 255.0f + 0.5f);
            int eb = static_cast<int>(kBars[b].b * 255.0f + 0.5f);
            int tol = 35;
            bool ok = std::abs(r - er) <= tol && std::abs(g - eg) <= tol &&
                      std::abs(bl - eb) <= tol;
            if (!ok) {
                std::printf("  FAIL frame %lu bar %d: got (%3d,%3d,%3d) expect (%3d,%3d,%3d)\n",
                           static_cast<unsigned long>(f->seq), b, r, g, bl, er, eg, eb);
                ++failures;
            }
        }
        ++checked;
    }

    if (failures) {
        std::printf("  FAIL: %d frame/bar combinations out of tolerance (checked %d frames)\n",
                   failures, checked);
        return 1;
    }
    
    if (check_all_frames && checked == 1) {
        // Original behavior: only checked last frame, now we check all
        std::printf("  OK: %u bar checks passed across %u frames\n", 7 * checked, checked);
    } else {
        std::printf("  OK: passed (checked %u frames)\n", checked);
    }
    
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    int fields = 30;
    int repeat = 1;
    double sample_rate = kDefaultFs;
    double dev_hz = kDefaultDev;
    bool check_all_frames = false;
    int width = 640;
    int height = 480;
    std::string output_path;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        
        if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else if (a == "--fields") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --fields requires a value\n");
                return 2;
            }
            fields = std::atoi(argv[++i]);
            if (fields <= 0) {
                std::fprintf(stderr, "error: --fields must be positive\n");
                return 2;
            }
        } else if (a == "--repeat") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --repeat requires a value\n");
                return 2;
            }
            repeat = std::atoi(argv[++i]);
            if (repeat <= 0) {
                std::fprintf(stderr, "error: --repeat must be positive\n");
                return 2;
            }
        } else if (a == "--rate") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --rate requires a value\n");
                return 2;
            }
            sample_rate = std::atof(argv[++i]);
            if (sample_rate <= 0) {
                std::fprintf(stderr, "error: --rate must be positive\n");
                return 2;
            }
        } else if (a == "--dev") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --dev requires a value\n");
                return 2;
            }
            dev_hz = std::atof(argv[++i]);
            if (dev_hz <= 0) {
                std::fprintf(stderr, "error: --dev must be positive\n");
                return 2;
            }
        } else if (a == "--check-frames") {
            check_all_frames = true;
        } else if (a == "--width") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --width requires a value\n");
                return 2;
            }
            width = std::atoi(argv[++i]);
            if (width <= 0) {
                std::fprintf(stderr, "error: --width must be positive\n");
                return 2;
            }
        } else if (a == "--height") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --height requires a value\n");
                return 2;
            }
            height = std::atoi(argv[++i]);
            if (height <= 0) {
                std::fprintf(stderr, "error: --height must be positive\n");
                return 2;
            }
        } else {
            // Treat as output file path
            output_path = a;
        }
    }

    if (!output_path.empty()) {
        // Write synthetic IQ to file and exit
        const size_t total = static_cast<size_t>(fields * kLinesPerField *
                                                 kLineUs * sample_rate / 1e6);
        std::vector<uint8_t> iq(total * 2);
        const double omega_sc = 2.0 * M_PI * kFsc / sample_rate;
        const double samples_per_line = sample_rate * kLineUs / 1e6;
        const double samples_per_field = samples_per_line * kLinesPerField;
        double phase = 0.0;
        for (size_t n = 0; n < total; ++n) {
            double in_field = std::fmod(static_cast<double>(n), samples_per_field);
            int line = static_cast<int>(in_field / samples_per_line);
            double us = std::fmod(in_field, samples_per_line) / sample_rate * 1e6;
            double theta = std::fmod(omega_sc * static_cast<double>(n), 2.0 * M_PI);
            float ire = composite_ire(line, us, theta);
            double f = dev_hz * ((ire + 40.0) / 70.0 - 1.0);
            phase = std::fmod(phase + 2.0 * M_PI * f / sample_rate, 2.0 * M_PI);
            auto clip8 = [](double x) {
                return static_cast<int8_t>(std::lround(std::fmax(-127.0, std::fmin(127.0, x))));
            };
            iq[2 * n] = static_cast<uint8_t>(clip8(100.0 * std::cos(phase)));
            iq[2 * n + 1] = static_cast<uint8_t>(clip8(100.0 * std::sin(phase)));
        }

        std::FILE* fp = std::fopen(output_path.c_str(), "wb");
        if (!fp) {
            std::fprintf(stderr, "error: cannot open %s for writing\n", output_path.c_str());
            return 1;
        }
        std::fwrite(iq.data(), 1, iq.size(), fp);
        std::fclose(fp);
        std::printf("wrote %zu IQ samples (%.1f MB) to %s (%d fields, %.1f MSPS)\n",
                   total, total * 2.0 / 1e6, output_path.c_str(), fields, sample_rate / 1e6);
        return 0;
    }

    // Run test(s)
    int total_failures = 0;
    for (int r = 0; r < repeat; ++r) {
        std::printf("Test %d/%d (%d fields, %.1f MSPS, %.1f MHz dev): ",
                   r + 1, repeat, fields, sample_rate / 1e6, dev_hz / 1e6);
        if (r > 0) std::printf("\n");
        
        int result = run_test(fields, sample_rate, dev_hz, check_all_frames, width, height);
        if (result != 0) {
            ++total_failures;
        }
    }

    if (total_failures > 0) {
        std::printf("FAIL: %d/%d test(s) failed\n", total_failures, repeat);
        return 1;
    }

    if (repeat == 1) {
        std::printf("PASS\n");
    } else {
        std::printf("PASS: all %d tests passed\n", repeat);
    }
    
    return 0;
}
