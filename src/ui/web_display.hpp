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

    // Initialize the web display server. Returns true on success.
    // port: HTTP server port (default 8080)
    bool init(int port = 8080, const std::string& title = "fpvdec");

    // Request a quit from the web UI (browser close or quit button)
    void request_quit();

    // Set the frame rate limit for the server thread
    void set_target_fps(int fps);

    // Get the current server state
    bool is_running() const { return running_.load(); }
    int port() const { return port_; }

    // Get the server URL for logging
    std::string get_url() const;

    // Called from main loop to update the latest frame and stats.
    void update_frame(const Frame* frame);
    void update_stats(const OsdStats& stats);

private:
    void server_thread_func();
    void apply_config(const std::string& key, const std::string& value);

    int port_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_requested_{false};
    int target_fps_ = 30;

    std::mutex frame_mutex_;
    std::unique_ptr<Frame> current_frame_;
    std::unique_ptr<OsdStats> current_stats_;
    std::chrono::steady_clock::time_point last_stats_update_;

    std::thread server_thread_;
};

} // namespace famidec
