// WebDisplay stub — Phase 1 infrastructure only
// Full implementation (HTML template, JPEG encoding, config wiring)
// will be done in Phase 2.

#include "web_display.hpp"

#include <cstdio>

#ifdef HAVE_WEBGUI
#include "httplib.h"
#endif

namespace famidec {

WebDisplay::WebDisplay() : port_(8080) {}

WebDisplay::~WebDisplay() {
    if (running_.load()) {
        running_.store(false);
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
}

#ifdef HAVE_WEBGUI
bool WebDisplay::init(int port, const std::string& /* title */) {
    port_ = port;
    quit_requested_.store(false);

    std::printf("Web GUI starting on port %d\n", port_);
    std::printf("Open http://localhost:%d/ in your browser\n", port_);
    std::printf("Press Ctrl+C to stop\n");
    std::fflush(stdout);

    running_.store(true);
    server_thread_ = std::thread(&WebDisplay::server_thread_func, this);
    return true;
}
#endif

#ifndef HAVE_WEBGUI
bool WebDisplay::init(int, const std::string&) {
    std::fprintf(stderr, "error: Web GUI not compiled in (rebuild with -DWEBGUI=ON)\n");
    return false;
}
#endif

void WebDisplay::request_quit() { quit_requested_.store(true); }

void WebDisplay::set_target_fps(int fps) { target_fps_ = fps; }

std::string WebDisplay::get_url() const {
    return "http://localhost:" + std::to_string(port_) + "/";
}

#ifdef HAVE_WEBGUI
void WebDisplay::server_thread_func() {
    httplib::Server svr;

    // Stub handlers — full implementation in Phase 2
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("Web GUI stub — implement serve_index in Phase 2", "text/plain");
        res.status = 501;
    });

    svr.Get("/api/frame", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("Frame endpoint stub — implement serve_frame in Phase 2", "text/plain");
        res.status = 501;
    });

    svr.Get("/api/stats", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{}", "application/json");
        res.status = 501;
    });

    svr.Post("/api/set", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });

    if (svr.listen("0.0.0.0", port_)) {
        while (running_.load() && !quit_requested_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    running_.store(false);
}
#else
void WebDisplay::server_thread_func() {
    // No-op when WebGUI not compiled in
}
#endif

void WebDisplay::update_frame(const Frame* frame) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame) {
        current_frame_ = std::make_unique<Frame>();
        current_frame_->rgba = frame->rgba;
        current_frame_->width = frame->width;
        current_frame_->height = frame->height;
        current_frame_->seq = frame->seq;
    }
}

void WebDisplay::update_stats(const OsdStats& stats) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_stats_update_).count();
    if (!current_stats_ || elapsed > 1) {
        current_stats_ = std::make_unique<OsdStats>();
        *current_stats_ = stats;
        last_stats_update_ = now;
    }
}

#ifdef HAVE_WEBGUI
void WebDisplay::apply_config(const std::string& key, const std::string& value) {
    std::printf("Web config change: %s = %s\n", key.c_str(), value.c_str());
    std::fflush(stdout);
    // Phase 2: apply to Config struct via callback
}
#else
void WebDisplay::apply_config(const std::string&, const std::string&) {}
#endif

} // namespace famidec
