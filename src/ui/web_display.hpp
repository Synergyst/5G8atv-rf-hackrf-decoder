#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../config.hpp"
#include "../dsp/frame.hpp"
#include "../source/sample_source.hpp"  // ISampleSource
#include "sdl_display.hpp"              // OsdStats

namespace famidec {

// Web display server — headless browser-based control panel.
// Provides an HTTP server that serves a browser UI with real-time
// video frames and decoder stats. Uses cpp-httplib for the HTTP layer.
//
// Build with: cmake -DWEBGUI=ON
// Usage: ./build/fpvdec --gui web --web-port 8080

class WebDisplay {
public:
    WebDisplay();
    ~WebDisplay();

    bool init(int port = 8080, const std::string& title = "fpvdec");
    void request_quit();
    void set_target_fps(int fps);

    bool is_running() const { return running_.load(); }
    int port() const { return port_; }
    std::string get_url() const;

    void update_frame(const Frame* frame);
    void update_stats(const OsdStats& stats);

    // Wire in a pointer to the live Config and the ISampleSource
    // so apply_config() can write hardware state on /api/set calls.
    void set_source_and_config(Config* cfg, ISampleSource* src) {
        cfg_ = cfg;
        source_ = src;
    }

private:
    void server_thread_func();

    // Apply a config change from the /api/set POST endpoint.
    // Writes directly into cfg_ and calls source_ hardware methods.
    void apply_config(const std::string& key, const std::string& value);

    int port_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_requested_{false};
    int target_fps_ = 30;
    int jpeg_quality_ = 75;

    std::mutex frame_mutex_;
    std::unique_ptr<Frame> current_frame_;
    std::unique_ptr<OsdStats> current_stats_;
    std::chrono::steady_clock::time_point last_stats_update_;

    std::thread server_thread_;

    // Wired in by set_source_and_config().
    Config* cfg_ = nullptr;
    ISampleSource* source_ = nullptr;
};

} // namespace famidec
