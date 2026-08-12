#pragma once

#include <functional>
#include <mutex>
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

    Config& unsafe_config() { return config_; }
    const Config& unsafe_config() const { return config_; }

private:
    Config& config_;
    mutable std::mutex mutex_;
};

} // namespace famidec
