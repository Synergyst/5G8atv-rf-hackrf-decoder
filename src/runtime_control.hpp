#pragma once

#include <functional>
#include <mutex>
#include <algorithm>
#include <utility>
#include "config.hpp"

namespace famidec {

// Single synchronization boundary for mutable runtime configuration. The
// control/UI thread owns mutations; DSP and lifecycle code take snapshots or
// short-lived read locks. Hardware/DSP work must never run while this lock is
// held.
class RuntimeControl {
public:
    explicit RuntimeControl(Config& config) : config_(config) {}

    RuntimeControl(const RuntimeControl&) = delete;
    RuntimeControl& operator=(const RuntimeControl&) = delete;

    template <typename Fn>
    decltype(auto) with_config(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::invoke(std::forward<Fn>(fn), config_);
    }

    template <typename Fn>
    decltype(auto) with_config(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::invoke(std::forward<Fn>(fn), config_);
    }

    std::unique_lock<std::mutex> lock() const {
        return std::unique_lock<std::mutex>(mutex_);
    }

    void set_event_queue(ConfigChangeQueue* queue) { event_queue_ = queue; }

    void submit(const ConfigChangeEvent& event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            switch (event.type) {
                case CFG_FM_DEV: config_.fm_dev_hz = std::clamp(event.val.dbl_val, 1e6, 10e6); break;
                case CFG_INVERT: config_.invert = event.val.bool_val; break;
                case CFG_SATURATION: config_.saturation = std::clamp(event.val.flt_val, 0.0f, 2.0f); break;
                case CFG_HUE_DEG: config_.hue_deg = std::clamp(event.val.flt_val, -180.0f, 180.0f); break;
                case CFG_OVERSCAN: config_.overscan = std::clamp(event.val.flt_val, 0.0f, 0.15f); break;
                case CFG_VIDEO_LPF: config_.video_lpf_hz = std::clamp(event.val.dbl_val, 0.0, 6e6); break;
                case CFG_AFC: config_.afc = event.val.bool_val; break;
                case CFG_DENOISE: config_.denoise = std::clamp(event.val.flt_val, 0.0f, 1.0f); break;
                case CFG_DENOISE_TEMPORAL: config_.denoise_temporal = std::clamp(event.val.flt_val, 0.0f, 1.0f); break;
                case CFG_DENOISE_MEDIAN: config_.denoise_temporal_median = std::clamp(event.val.int_val, 0, 9); break;
                case CFG_DENOINE_MEDIAN_STRENGTH: config_.denoise_temporal_median_strength = std::clamp(event.val.flt_val, 0.0f, 1.0f); break;
                case CFG_SAMPLE_RATE: config_.sample_rate = std::clamp(event.val.dbl_val, 6e6, 20e6); break;
                case CFG_SAMPLE_BITS: if (event.val.int_val == 8 || event.val.int_val == 16) config_.sample_bits = event.val.int_val; break;
                case CFG_VIDEO_CARRIER: config_.video_carrier_hz = std::clamp(event.val.dbl_val, 5.6e9, 6.0e9); break;
                case CFG_OFFSET_HZ: config_.offset_hz = std::clamp(event.val.dbl_val, -2e6, 2e6); break;
                case CFG_LNA_GAIN: config_.lna_gain = std::clamp((event.val.int_val / 8) * 8, 0, 40); break;
                case CFG_VGA_GAIN: config_.vga_gain = std::clamp((event.val.int_val / 2) * 2, 0, 62); break;
                case CFG_AMP: config_.amp = event.val.bool_val; break;
                case CFG_GAIN_AUTO: config_.gain_auto = event.val.bool_val; break;
                case CFG_FRAME_WIDTH: config_.frame_width = std::clamp(event.val.int_val, 320, 1920); break;
                case CFG_FRAME_HEIGHT: config_.frame_height = std::clamp(event.val.int_val, 240, 1080); break;
                case CFG_AUTO_DETECT: config_.auto_detect = event.val.bool_val; break;
                case CFG_CLkout: config_.clkout = event.val.bool_val; break;
                case CFG_ENFORCE_CLKIN: config_.enforce_clkin = event.val.bool_val; break;
            }
        }
        if (event_queue_) event_queue_->push(event);
    }

    Config snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

    Config& unsafe_config() { return config_; }
    const Config& unsafe_config() const { return config_; }

private:
    Config& config_;
    mutable std::mutex mutex_;
    ConfigChangeQueue* event_queue_ = nullptr;
};

} // namespace famidec
