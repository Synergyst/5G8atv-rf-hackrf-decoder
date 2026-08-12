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
#include "runtime_auto_resolution.hpp"
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

namespace famidec {



namespace {
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

using Recorder = RuntimeRecorder;

double carrier_offset_hz(const NtscDecoder& dec, const Config& cfg) {
    double sgn = cfg.invert ? 1.0 : -1.0;
    double f_tip = sgn * dec.stats().agc_tip_raw.load() * cfg.fm_dev_hz;
    double f_blank = sgn * dec.stats().agc_blank_raw.load() * cfg.fm_dev_hz;
    double f_white = f_blank + 2.5 * (f_blank - f_tip);
    return 0.5 * (f_tip + f_white);
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


int run_application(Config& cfg, const Config& startup_baseline) {
    RuntimeControl runtime(cfg);
    RuntimeLifecycle lifecycle;
    RuntimeSession session(cfg, runtime, lifecycle);

    lifecycle.start();
    RuntimeCoordinator coordinator(cfg, runtime, lifecycle, lifecycle.running_ref());
    if (!coordinator.create_source(!cfg.headless && !cfg.spectrum)) {
        std::fprintf(stderr, "failed to create input source\n");
        return 1;
    }
    ISampleSource* src = coordinator.source();
    HackRfSource* hackrf = dynamic_cast<HackRfSource*>(src);
#ifdef HAVE_SOAPYSDR
    SoapySource* soapysdr = dynamic_cast<SoapySource*>(src);
#endif
#ifdef HAVE_UHD
    UhdSource* uhd = dynamic_cast<UhdSource*>(src);
#endif
    if (!coordinator.start_source()) {
        std::fprintf(stderr, "failed to start input source: %s\n", src->error().c_str());
        return 1;
    }
    if (cfg.sample_bits == 16 && src->sample_format() != SampleFormat::CS16) {
        std::fprintf(stderr, "error: --bits 16 is only supported by UHD/SoapySDR sources; this source provides %s\n",
                     sample_format_name(src->sample_format()));
        coordinator.stop_source();
        return 2;
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
    std::string source_name;
    if (cfg.input == Config::Input::HackRF) source_name = "HackRF";
#ifdef HAVE_SOAPYSDR
    else if (cfg.input == Config::Input::SoapySDR) {
        source_name = "SoapySDR";
        if (soapysdr) source_name += " (" + soapysdr->device_info() + ")";
    }
#endif
#ifdef HAVE_UHD
    else if (cfg.input == Config::Input::UHD) {
        source_name = "UHD";
        if (uhd) source_name += " (" + uhd->device_info() + ")";
    }
#endif
    else source_name = cfg.file_path;
    std::printf("input: %s   format %s   video carrier %.3f MHz   center %.3f MHz   %.1f MSPS\n", source_name.c_str(), sample_format_name(src->sample_format()), cfg.video_carrier_hz / 1e6, cfg.center_hz() / 1e6, cfg.sample_rate / 1e6);

    if (cfg.spectrum) {
        int rc = run_spectrum(cfg, src);
        coordinator.stop_source();
        return rc;
    }

    Recorder rec;
    if (!cfg.record_path.empty() && !rec.start(cfg.record_path)) {
        std::fprintf(stderr, "cannot open %s\n", cfg.record_path.c_str());
        return 1;
    }

    TripleBuffer tb;
    NtscDecoder* dec = nullptr;
    std::atomic<float> mean_raw{0.0f};

    // Auto-resolution: detect before starting DSP thread
    Config auto_cfg = cfg;
    if (cfg.auto_detect && !cfg.auto_res_applied) {
        // Start DSP briefly to detect resolution
        TripleBuffer tmp_tb;
        tmp_tb.resize(cfg.frame_width, cfg.frame_height);
        NtscDecoder tmp_dec(auto_cfg, tmp_tb);
        std::atomic<float> tmp_mean_raw{0.0f};
        if (!coordinator.start_dsp(&tmp_dec, &rec, &tmp_mean_raw, nullptr)) {
            std::fprintf(stderr, "failed to start auto-detection DSP\n");
            coordinator.stop_source();
            return 1;
        }
        
        // Wait for detection (max 10 seconds)
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!tmp_dec.stats().auto_detect_ready.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Stop DSP thread and restart source stream cleanly. The source
        // pause/resume pair owns the backend-specific stream lifecycle.
        src->pause();
        coordinator.stop_dsp();
        src->resume();
        lifecycle.start();
        
        // Apply detected resolution
        double chroma_hz = tmp_dec.stats().detected_chroma_hz.load(std::memory_order_acquire);
        int active_lines = tmp_dec.stats().detected_active_lines.load(std::memory_order_acquire);
        int horiz_detail = tmp_dec.stats().detected_horiz_detail.load(std::memory_order_acquire);
        bool is_pal = (chroma_hz > 4.0e6);
        if (active_lines <= 0) active_lines = is_pal ? 288 : 240;
        
        AutoResolution::apply_aspect(auto_cfg, tmp_dec);
        const int target_width = auto_cfg.frame_width;
        const int target_height = auto_cfg.frame_height;
        
        std::printf("AUTO-RES: detected %s (chroma=%.2f MHz, %d active lines, "
                   "horiz_detail=%d, line_rate=%.3f kHz)\n",
                   is_pal ? "PAL" : "NTSC",
                   chroma_hz / 1e6, active_lines,
                   horiz_detail, tmp_dec.stats().detected_line_rate.load() / 1000.0);
        std::printf("  -> applying resolution: %dx%d\n", 
                   target_width, target_height);
        std::fflush(stdout);
    }

    // Copy auto-detected resolution into cfg so the GUI path's
    // tb.resize(cfg.frame_width, ...) below uses the correct dimensions.
    cfg.frame_width = auto_cfg.frame_width;
    cfg.frame_height = auto_cfg.frame_height;
    cfg.auto_res_applied = true;
    
    session.prepare_frames(tb);
    dec = new NtscDecoder(cfg, tb);
    dec->set_saturation(cfg.saturation);
    dec->set_hue_deg(cfg.hue_deg);
    dec->set_overscan(cfg.overscan);
    dec->set_color_mode(cfg.mode == Config::Mode::Color);
    ConfigChangeQueue web_events_store;
    ConfigChangeQueue* web_events = nullptr;
#ifdef HAVE_WEBGUI
    web_events = &web_events_store;
#endif
    runtime.set_event_queue(web_events);
    if (!coordinator.start_dsp(dec, &rec, &mean_raw, web_events)) {
        std::fprintf(stderr, "failed to start DSP coordinator\n");
        return 1;
    }

    int rc = 0;

    // Debug mode: run DSP without GUI, print periodic stats to stdout.
    // Supports Ctrl+C (SIGINT) and --debug-duration N for finite runs.
    if (cfg.debug_mode) {
        // A restart request is handled by the same outer lifecycle loop as
        // WebUI; debug mode must not continue using stale DSP state.
        // SIGINT handler: set g_running false so the DSP thread exits cleanly.
        std::signal(SIGINT, [](int) { /* lifecycle is stopped by the main loop */ });

        uint64_t prev_frames = 0;
        uint64_t prev_lines = 0;
        auto last_stat = std::chrono::steady_clock::now();
        auto run_start = std::chrono::steady_clock::now();
        uint64_t last_dropped = 0;
        uint64_t last_clipped = 0;

        std::printf("[DEBUG MODE] Running... Ctrl+C to stop.\n");
        std::fflush(stdout);

        while (lifecycle.running()) {
            // Check duration limit
            if (cfg.debug_duration_sec > 0) {
                auto elapsed = std::chrono::steady_clock::now() - run_start;
                if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= cfg.debug_duration_sec) {
                    std::printf("\n[DEBUG MODE] Duration limit reached (%d seconds).\n", cfg.debug_duration_sec);
                    std::fflush(stdout);
                    break;
                }
            }

            // Acquire frames (non-blocking)
            const Frame* f = tb.acquire();
            (void)f; // just to confirm frames are flowing

            // Print stats periodically
            auto now = std::chrono::steady_clock::now();
            if (now - last_stat >= std::chrono::seconds(1)) {
                uint64_t frames = dec->stats().frames.load();
                uint64_t lines = dec->stats().lines.load();
                uint64_t dropped = src->dropped_bytes();
                uint64_t clipped = src->clipped_samples();

                // Calculate rates
                double dt = std::chrono::duration<double>(now - last_stat).count();
                double fps = dt > 0 ? (frames - prev_frames) / dt : 0.0;
                double lps = dt > 0 ? (lines - prev_lines) / dt : 0.0;
                double drop_rate = dt > 0 ? (dropped - last_dropped) / dt : 0.0;
                double clip_rate = dt > 0 ? (clipped - last_clipped) / dt : 0.0;

                bool line_locked = dec->stats().line_locked.load();
                bool vsync_locked = (now - last_stat < std::chrono::milliseconds(500));

                std::printf("[STATS] frames=%llu lines=%llu fps=%.1f lps=%.1f "
                           "line=%s vsync=%s dropped=%.0f/s clipped=%.0f/s "
                           "ring=%.1f%%\n",
                           static_cast<unsigned long long>(frames),
                           static_cast<unsigned long long>(lines),
                           fps, lps,
                           line_locked ? "LOCKED" : "lost",
                           vsync_locked ? "locked" : "lost",
                           drop_rate, clip_rate,
                           src->ring_fill());
                std::fflush(stdout);

                prev_frames = frames;
                prev_lines = lines;
                last_stat = now;
                last_dropped = dropped;
                last_clipped = clipped;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (lifecycle.restart_requested()) {
            lifecycle.clear();
            lifecycle.request_quit();
            coordinator.stop_dsp();
            coordinator.stop_source();
            return 75;
        }

        // Final summary
        auto duration = std::chrono::steady_clock::now() - run_start;
        double total_secs = std::chrono::duration<double>(duration).count();
        uint64_t total_frames = dec->stats().frames.load();
        uint64_t total_lines = dec->stats().lines.load();
        uint64_t total_coasted = dec->stats().lines_coasted.load();

        std::printf("\n[DEBUG SUMMARY] Duration: %.1f seconds\n", total_secs);
        std::printf("  Frames: %llu\n", static_cast<unsigned long long>(total_frames));
        std::printf("  Lines: %llu (coasted: %llu)\n",
                    static_cast<unsigned long long>(total_lines),
                    static_cast<unsigned long long>(total_coasted));
        std::printf("  Dropped bytes: %llu\n",
                    static_cast<unsigned long long>(src->dropped_bytes()));
        std::printf("  Clipped samples: %llu\n",
                    static_cast<unsigned long long>(src->clipped_samples()));
        if (dec->stats().line_locked.load()) {
            std::printf("  Carrier offset: %+.2f MHz\n",
                        carrier_offset_hz(*dec, cfg) / 1e6);
        }
        if (total_frames == 0) {
            std::printf("  WARNING: No frames decoded - check signal/source\n");
        }
        std::fflush(stdout);
    } else if (cfg.headless) {
        uint64_t last_seq = 0;
        int written = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (written < cfg.dump_frame_count && lifecycle.running() && std::chrono::steady_clock::now() < deadline) {
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
        if (lifecycle.restart_requested()) {
            lifecycle.clear();
            lifecycle.request_quit();
            coordinator.stop_dsp();
            coordinator.stop_source();
            return 75;
        }
        std::printf("wrote %d frames; decoded lines=%llu coasted=%llu frames=%llu\n",
                    written,
                    static_cast<unsigned long long>(dec->stats().lines.load()),
                    static_cast<unsigned long long>(dec->stats().lines_coasted.load()),
                    static_cast<unsigned long long>(dec->stats().frames.load()));
        if (dec->stats().line_locked.load())
            std::printf("carrier offset: %+.2f MHz from tuned center (live AFC would re-tune; or use --freq)\n",
                        carrier_offset_hz(*dec, cfg) / 1e6);
        if (written == 0) rc = 1;
    } else {
#ifdef HAVE_WEBGUI
        if (cfg.gui_mode == Config::GuiMode::Web) {
            // Web GUI mode — headless server with browser display
            WebDisplay web_disp;
            if (!web_disp.init(cfg.web_port)) {
                std::fprintf(stderr, "Web GUI init failed\n");
                lifecycle.request_quit();
                coordinator.stop_dsp();
                return 1;
            }
            // Wire in config + source so /api/set can control hardware.
            web_disp.set_source_and_config(src, &startup_baseline, &runtime, &lifecycle);
            web_disp.set_config_queue(&web_events_store);
            web_disp.set_config_queue(&web_events_store);

            // Build the filter pipeline
            FilterPipeline pipeline;
            RuntimeModeSupport::build_filters(cfg, pipeline);
            float last_denoise = cfg.denoise;
            float last_denoise_temporal = cfg.denoise_temporal;
            int last_denoise_median = cfg.denoise_temporal_median;
            float last_denoise_median_strength = cfg.denoise_temporal_median_strength;
            int last_pipeline_width = cfg.frame_width;
            int last_pipeline_height = cfg.frame_height;

            auto rebuild_web_pipeline = [&]() {
                RuntimeModeSupport::build_filters(cfg, pipeline);
                last_denoise = cfg.denoise;
                last_denoise_temporal = cfg.denoise_temporal;
                last_denoise_median = cfg.denoise_temporal_median;
                last_denoise_median_strength = cfg.denoise_temporal_median_strength;
                last_pipeline_width = cfg.frame_width;
                last_pipeline_height = cfg.frame_height;
                std::printf("WebGUI: filter pipeline rebuilt (denoise=%.2f temporal=%.2f median=%d strength=%.2f)\n",
                            cfg.denoise, cfg.denoise_temporal,
                            cfg.denoise_temporal_median,
                            cfg.denoise_temporal_median_strength);
            };

            std::string channel = fpv_nearest_channel(cfg.video_carrier_hz);
            uint64_t prev_frames = 0;
            auto last_frame_inc = std::chrono::steady_clock::now();
            auto last_afc = last_frame_inc;
            auto lock_changed = last_frame_inc;
            bool was_locked = false;
            std::vector<double> afc_meas;
            auto last_agc = last_frame_inc;
            auto last_clip_seen = last_frame_inc - std::chrono::seconds(10);
            uint64_t last_clip_count = 0;

            while (lifecycle.running()) {
                auto config_lock = runtime.lock();
                if (web_disp.restart_requested() || lifecycle.restart_requested()) {
                    config_lock.unlock();
                    std::printf("WebGUI: full application reinitialization requested\n");
                    lifecycle.clear();
                    lifecycle.request_quit();
                    web_disp.request_quit();
                    coordinator.stop_dsp();
                    std::string restart_path; uint64_t restart_bytes = 0;
                    rec.stop(&restart_path, &restart_bytes);
                    return 75;
                }
                if (cfg.denoise != last_denoise ||
                    cfg.denoise_temporal != last_denoise_temporal ||
                    cfg.denoise_temporal_median != last_denoise_median ||
                    cfg.denoise_temporal_median_strength != last_denoise_median_strength) {
                    rebuild_web_pipeline();
                }
                // Acquire frame
                const Frame* f = tb.acquire();
                if (f) {
                    if (cfg.auto_detect && !cfg.auto_res_applied &&
                        dec->stats().auto_detect_ready.load(std::memory_order_acquire)) {
                        // Auto-res detection — same as SDL mode
                        double chroma_hz = dec->stats().detected_chroma_hz.load(std::memory_order_acquire);
                        int active_lines = dec->stats().detected_active_lines.load(std::memory_order_acquire);
                        int horiz_detail = dec->stats().detected_horiz_detail.load(std::memory_order_acquire);
                        int line_rate_mhz = dec->stats().detected_line_rate.load(std::memory_order_acquire);
                        bool is_pal = (chroma_hz > 4.0e6);
                        if (active_lines <= 0) active_lines = is_pal ? 288 : 240;
                        AutoResolution::apply_aspect(cfg, *dec);
                        cfg.auto_res_applied = true;
                        // Resolution changes are structural. Let the common
                        // restart path rebuild buffers and filters after all
                        // producer threads have stopped.
                        lifecycle.request_restart();
                        std::printf("AUTO-RES: detected %s (chroma=%.2f MHz, %d active lines, "
                                   "horiz_detail=%d, line_rate=%.3f kHz)\n  -> %dx%d\n",
                                   is_pal ? "PAL" : "NTSC", chroma_hz / 1e6, active_lines,
                                   horiz_detail, line_rate_mhz / 1000.0,
                                   cfg.frame_width, cfg.frame_height);
                        std::fflush(stdout);
                    }

                    // Build stats
                    OsdStats st;
                    st.line_locked = dec->stats().line_locked.load();
                    st.burst_amp = dec->stats().burst_amp.load();
                    st.ring_fill = src->ring_fill();
                    st.dropped = src->dropped_bytes();
                    st.clipped = src->clipped_samples();
                    st.frames = dec->stats().frames.load();
                    st.lines = dec->stats().lines.load();
                    if (hackrf) { st.lna = hackrf->lna(); st.vga = hackrf->vga(); }
                    st.amp = cfg.amp;
                    st.gain_auto = cfg.gain_auto;
                    st.clkin_locked = hackrf ? hackrf->check_clkin() : false;

                    auto now = std::chrono::steady_clock::now();
                    if (st.clipped > last_clip_count) { last_clip_count = st.clipped; last_clip_seen = now; }
                    st.clipping = (now - last_clip_seen) < std::chrono::seconds(1);
                    if (cfg.gain_auto && hackrf && now - last_agc >= std::chrono::milliseconds(500)) {
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
                        if (hackrf->lna() != lna || hackrf->vga() != vga)
                            std::printf("AGC: peak %d -> LNA%d VGA%d\n", peak, hackrf->lna(), hackrf->vga());
                    }
                    // FPS: measure from actual frame delivery rate
                    auto now2 = std::chrono::steady_clock::now();
                    double fps_delta = std::chrono::duration<double>(now2 - last_frame_inc).count();
                    st.fps = (fps_delta > 0) ? static_cast<float>(1.0 / fps_delta) : 0.0f;
                    if (st.frames > prev_frames) { prev_frames = st.frames; last_frame_inc = now2; }
                    st.vsync_locked = (now2 - last_frame_inc) < std::chrono::milliseconds(500);
                    st.freq_mhz = cfg.video_carrier_hz / 1e6;
                    st.channel = channel;
                    st.video_latency_ms = 0;  // no reliable latency calc in Web mode

                    if (cfg.frame_width != last_pipeline_width || cfg.frame_height != last_pipeline_height) {
                        // Resolution changes are structural. Restart the whole
                        // application rather than resizing buffers while DSP
                        // may still be writing into a published frame.
                        lifecycle.request_restart();
                        continue;
                    }

                    // Config state for Web GUI read-back
                    st.afc_enabled = cfg.afc;
                    st.fm_dev_hz = cfg.fm_dev_hz;
                    st.invert = cfg.invert;
                    st.video_lpf_hz = cfg.video_lpf_hz;
                    st.saturation = cfg.saturation;
                    st.hue_deg = cfg.hue_deg;
                    st.offset_hz = cfg.offset_hz;
                    st.sample_rate = cfg.sample_rate;
                    st.frame_width = cfg.frame_width;
                    st.frame_height = cfg.frame_height;
                    st.auto_detect = cfg.auto_detect;

                    // AFC
                    {
                        bool locked = st.line_locked && st.vsync_locked;
                        if (locked != was_locked) { was_locked = locked; lock_changed = now; afc_meas.clear(); }
                        if (cfg.afc && src && now - last_afc >= std::chrono::milliseconds(500) &&
                            now - lock_changed >= std::chrono::seconds(2)) {
                            last_afc = now;
                            afc_meas.push_back(locked ? carrier_offset_hz(*dec, cfg) :
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
                                    cfg.video_carrier_hz += step;
                                    src->set_center_freq(cfg.center_hz());
                                    dec->shift_levels(static_cast<float>((cfg.invert ? -step : step) / cfg.fm_dev_hz));
                                    channel = fpv_nearest_channel(cfg.video_carrier_hz);
                                    std::printf("AFC%s: %+0.2f MHz -> %.2f MHz\n", locked ? "" : " (coarse)", step / 1e6, cfg.video_carrier_hz / 1e6);
                                    std::fflush(stdout);
                                }
                            }
                        }
                    }

                    // Update WebDisplay
                    web_disp.update_frame(f);
                    web_disp.update_stats(st);

                    // Apply filter pipeline to frame for Web display.
                    // Push filtered frame; if no filters, use the raw frame.
                    if (pipeline.empty()) {
                        web_disp.update_frame(f);
                    } else {
                        Frame filtered;
                        filtered.rgba = f->rgba;
                        filtered.width = f->width;
                        filtered.height = f->height;
                        pipeline.process(filtered);
                        web_disp.update_frame(&filtered);
                    }
                }

                // Check quit
                if (web_disp.is_running()) {
                    // Server running
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            lifecycle.request_quit();
            web_disp.request_quit();
            coordinator.stop_dsp();
            std::string rec_path; uint64_t rec_bytes = 0;
            if (rec.stop(&rec_path, &rec_bytes))
                std::printf("saved %s (%.1f MB) - replay: fpvdec --input file --file %s\n", rec_path.c_str(), rec_bytes / 1e6, rec_path.c_str());
            return rc;
        }
        // else: fall through to SDL/ImGui
#endif  // HAVE_WEBGUI

        const bool use_imgui = (cfg.gui_mode == Config::GuiMode::ImGui);
        SdlDisplay disp;
        if (!disp.init("fpvdec - FPV ATV decoder", cfg.frame_width, cfg.frame_height)) {
            std::fprintf(stderr, "SDL init failed\n");
            lifecycle.request_quit();
            coordinator.stop_dsp();
            return 1;
        }
        // The decoder/frame buffers were sized before DSP start. Do not resize
        // TripleBuffer from the UI loop; structural changes restart the run.

        GuiManager gui;
        if (use_imgui) {
            gui.init(disp.win(), disp.renderer(), cfg);
            disp.set_hotkeys_enabled(false);
        }

        Config& mcfg = cfg;

        // Build the filter pipeline from config.
        // Filters are auto-registered via REGISTER_FILTER() macro.
        FilterPipeline pipeline;
        RuntimeModeSupport::build_filters(mcfg, pipeline);
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
        while (lifecycle.running()) {
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
                else if (rec.start(famidec::next_recording_path()))
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
                src->set_center_freq(mcfg.center_hz());
                channel = fpv_nearest_channel(mcfg.video_carrier_hz);
            }
            if (act == KeyAction::ToggleColor) {
                mcfg.mode = (mcfg.mode == Config::Mode::Color) ? Config::Mode::Gray : Config::Mode::Color;
                dec->set_color_mode(mcfg.mode == Config::Mode::Color);
            }

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
            st.line_locked = dec->stats().line_locked.load();
            st.burst_amp = dec->stats().burst_amp.load();
            st.ring_fill = src->ring_fill();
            st.dropped = src->dropped_bytes();
            st.clipped = src->clipped_samples();
            st.frames = dec->stats().frames.load();
            st.lines = dec->stats().lines.load();
            if (hackrf) { st.lna = hackrf->lna(); st.vga = hackrf->vga(); }
            st.amp = mcfg.amp;
            st.gain_auto = mcfg.gain_auto;
            st.clkin_locked = hackrf ? hackrf->check_clkin() : false;
            
            // Auto-resolution: apply detected resolution after lock is acquired
            if (mcfg.auto_detect && !mcfg.auto_res_applied && 
                dec->stats().auto_detect_ready.load(std::memory_order_acquire)) {
                double chroma_hz = dec->stats().detected_chroma_hz.load(std::memory_order_acquire);
                int active_lines = dec->stats().detected_active_lines.load(std::memory_order_acquire);
                int horiz_detail = dec->stats().detected_horiz_detail.load(std::memory_order_acquire);
                int line_rate_mhz = dec->stats().detected_line_rate.load(std::memory_order_acquire);
                
                // Determine standard from chroma frequency
                bool is_pal = (chroma_hz > 4.0e6);  // PAL ~4.43 MHz vs NTSC ~3.58 MHz
                
                // If active_lines wasn't counted (grayscale or no burst), use standard values
                if (active_lines <= 0) {
                    active_lines = is_pal ? 288 : 240;  // PAL vs NTSC standard
                }
                
                AutoResolution::apply_aspect(mcfg, *dec);
                // Resolution changes are structural. Stop the current run and
                // let the outer restart path rebuild all frame/display state.
                lifecycle.request_restart();
                std::printf("AUTO-RES: detected %s (chroma=%.2f MHz, %d active lines, "
                           "horiz_detail=%d, line_rate=%.3f kHz)\n  -> %dx%d\n",
                           is_pal ? "PAL" : "NTSC", chroma_hz / 1e6, active_lines,
                           horiz_detail, line_rate_mhz / 1000.0,
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
                if (cfg.afc && src && now - last_afc >= std::chrono::milliseconds(500) && now - lock_changed >= std::chrono::seconds(2)) {
                    last_afc = now;
                    afc_meas.push_back(locked ? carrier_offset_hz(*dec, cfg) :
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
                            src->set_center_freq(mcfg.center_hz());
                            dec->shift_levels(static_cast<float>((cfg.invert ? -step : step) / cfg.fm_dev_hz));
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
                uint64_t dsp_samples = dec->stats().samples_in.load();
                uint64_t vsync_pos = dec->stats().frame_sample_pos.load();
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
                // Apply filter pipeline if enabled
                if (pipeline.empty()) {
                    disp.render_video_only(f);
                } else {
                    Frame filtered;
                    filtered.resize(mcfg.frame_width, mcfg.frame_height);
                    filtered.rgba = f->rgba;
                    pipeline.process(filtered);
                    disp.render_video_only(&filtered);
                }
                gui_screenshot = gui.render(st);
                SDL_RenderPresent(disp.renderer());
                if (gui_screenshot > 0.0f && have_shown) {
                    char path[64]; std::snprintf(path, sizeof(path), "fpvdec_%03d.bmp", shot++);
                    disp.screenshot(last_shown, path);
                    std::printf("saved %s\n", path);
                }
            } else {
                // Apply filter pipeline if enabled
                if (pipeline.empty()) {
                    disp.render(f, st);
                } else {
                    Frame filtered;
                    filtered.resize(mcfg.frame_width, mcfg.frame_height);
                    filtered.rgba = f->rgba;
                    pipeline.process(filtered);
                    disp.render(&filtered, st);
                }
            }
        }
        if (use_imgui) gui.shutdown();
    }
    lifecycle.request_quit();
    coordinator.stop_source();
    coordinator.stop_dsp();
    std::string rec_path; uint64_t rec_bytes = 0;
    if (rec.stop(&rec_path, &rec_bytes))
        std::printf("saved %s (%.1f MB) - replay: fpvdec --input file --file %s\n", rec_path.c_str(), rec_bytes / 1e6, rec_path.c_str());
    return rc;
}

} // namespace famidec
