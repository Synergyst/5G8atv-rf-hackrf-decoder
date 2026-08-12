#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../config.hpp"
#include "../dsp/ring_buffer.hpp"
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
        return ring_.readable();
    }
    uint64_t clipped_samples() const override {
        return clipped_.load(std::memory_order_relaxed);
    }
    float ring_fill() const override;
    SampleFormat sample_format() const override { return format_; }

    bool set_gains(int lna, int vga) override;
    bool set_center_freq(double center_hz) override;
    bool restart() override;
    int lna() const { return lna_; }
    int vga() const { return vga_; }

    const std::string& device_info() const { return device_info_; }
    static std::vector<std::string> enumerate_devices();
    const std::string& error() const override { return error_; }
    bool failed() const override { return !error_.empty() && !running_.load(std::memory_order_acquire); }

private:
    bool configure_gains_locked();
    bool set_gains_locked(int lna, int vga);
    void rx_loop();

    const Config& cfg_;
    std::string device_args_;

    SoapySDR::Device* device_ = nullptr;
    SoapySDR::Stream* stream_ = nullptr;
    SpscRing ring_;
    SampleFormat format_ = SampleFormat::CS8;

    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> clipped_{0};
    std::atomic<bool> running_{false};
    std::thread worker_;

    int lna_{20}, vga_{20};
    std::string device_info_;
    std::string error_;
    mutable std::mutex mu_;
};

}  // namespace famidec
