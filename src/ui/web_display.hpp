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
#include "../runtime_control.hpp"
#include "../runtime_lifecycle.hpp"
#ifdef HAVE_WEBGUI
namespace httplib { class Server; }
#endif
#include "../dsp/frame.hpp"
#include "../source/sample_source.hpp"
#include "sdl_display.hpp"

namespace famidec {

class WebDisplay {
public:
    WebDisplay();
    ~WebDisplay();

    bool init(int port = 8080, const std::string& title = "fpvdec");
    void request_quit();
    void request_restart() {
        restart_requested_.store(true);
        if (lifecycle_) lifecycle_->request_restart();
    }
    void set_target_fps(int fps);

    bool is_running() const { return running_.load(); }
    bool restart_requested() const { return restart_requested_.load(); }
    int port() const { return port_; }
    std::string get_url() const;

    void update_frame(const Frame* frame);
    void update_stats(const OsdStats& stats);

    void set_source_and_config(ISampleSource* src,
                               const Config* reset_cfg = nullptr,
                               RuntimeControl* runtime = nullptr,
                               RuntimeLifecycle* lifecycle = nullptr) {
        source_ = src;
        if (reset_cfg) { reset_cfg_ = *reset_cfg; have_reset_cfg_ = true; }
        runtime_ = runtime;
        lifecycle_ = lifecycle;
    }

    void set_config_queue(ConfigChangeQueue* queue) {
        config_queue_ = queue;
    }

    std::unique_lock<std::mutex> lock_config() const {
        return runtime_ ? runtime_->lock() : std::unique_lock<std::mutex>();
    }


private:
    void server_thread_func();
    void apply_config(const std::string& key, const std::string& value);

    int port_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_requested_{false};
    std::atomic<bool> restart_requested_{false};
    int target_fps_ = 30;
    int jpeg_quality_ = 75;

    std::mutex frame_mutex_;
    std::unique_ptr<Frame> current_frame_;
    std::unique_ptr<OsdStats> current_stats_;
    std::chrono::steady_clock::time_point last_stats_update_;

    std::thread server_thread_;
#ifdef HAVE_WEBGUI
    std::mutex server_mutex_;
    std::unique_ptr<httplib::Server> server_;
#endif

    ISampleSource* source_ = nullptr;
    Config reset_cfg_{};
    bool have_reset_cfg_ = false;
    ConfigChangeQueue* config_queue_ = nullptr;
    RuntimeControl* runtime_ = nullptr;
    RuntimeLifecycle* lifecycle_ = nullptr;
};

} // namespace famidec
