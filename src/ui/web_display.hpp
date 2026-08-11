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
#include "../source/sample_source.hpp"
#include "sdl_display.hpp"

namespace famidec {

class WebDisplay {
public:
    WebDisplay();
    ~WebDisplay();

    bool init(int port = 8080, const std::string& title = "fpvdec");
    void request_quit();
    void request_restart() { restart_requested_.store(true); }
    void set_target_fps(int fps);

    bool is_running() const { return running_.load(); }
    bool restart_requested() const { return restart_requested_.load(); }
    int port() const { return port_; }
    std::string get_url() const;

    void update_frame(const Frame* frame);
    void update_stats(const OsdStats& stats);

    void set_source_and_config(Config* cfg, ISampleSource* src,
                               const Config* reset_cfg = nullptr) {
        cfg_ = cfg;
        source_ = src;
        reset_cfg_ = reset_cfg;
    }

    void set_config_queue(ConfigChangeQueue* queue) {
        config_queue_ = queue;
    }

private:
    void server_thread_func();
    void apply_config(const std::string& key, const std::string& value);
    bool reset_config();

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

    Config* cfg_ = nullptr;
    ISampleSource* source_ = nullptr;
    const Config* reset_cfg_ = nullptr;
    ConfigChangeQueue* config_queue_ = nullptr;
};

} // namespace famidec
