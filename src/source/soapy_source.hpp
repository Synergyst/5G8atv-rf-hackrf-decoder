#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config.hpp"
#include "dsp/ring_buffer.hpp"
#include "sample_source.hpp"

// Forward declaration for SoapySDR types
namespace SoapySDR { class Device; struct Stream; }

namespace famidec {

class SoapySource : public ISampleSource {
public:
    explicit SoapySource(const Config& cfg, const std::string& device_args = "");
    ~SoapySource() override;

    bool start() override;
    void stop() override;
    void pause() override;
    void resume() override;
    size_t read(uint8_t* buf, size_t len) override;

    uint64_t dropped_bytes() const override {
        return dropped_.load(std::memory_order_relaxed);
    }
    uint64_t total_bytes() const override {
        return total_.load(std::memory_order_relaxed);
    }
    uint64_t buffered_bytes() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return ring_.readable();
    }
    uint64_t clipped_samples() const override {
        return clipped_.load(std::memory_order_relaxed);
    }
    float ring_fill() const override;

    bool set_gains(int lna, int vga);
    bool set_center_freq(double center_hz);
    int lna() const { return lna_; }
    int vga() const { return vga_; }

    // Device info for display
    const std::string& device_info() const { return device_info_; }

    // List available devices
    static std::vector<std::string> enumerate_devices();

    const std::string& error() const { return error_; }

    bool configure_gains();

    const Config& cfg_;
    std::string device_args_;

    // SoapySDR handles
    SoapySDR::Device* device_ = nullptr;
    SoapySDR::Stream* stream_ = nullptr;

    // Ring buffer for data flow (USB thread fills, main thread reads)
    SpscRing ring_;

    // Stats
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> clipped_{0};
    std::atomic<bool> running_{false};

    // Gain state (approximate, device-dependent)
    int lna_{20}, vga_{20};

    // Device info for display
    std::string device_info_;

    // Error state
    std::string error_;
    mutable std::mutex mu_;
};

}  // namespace famidec
