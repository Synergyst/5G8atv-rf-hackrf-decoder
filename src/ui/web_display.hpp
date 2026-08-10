#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../config.hpp"
#include "../dsp/frame.hpp"
#include "sdl_display.hpp"  // For OsdStats

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

private:
    void server_thread_func();
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
};

} // namespace famidec
