#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../config.hpp"
#include "../dsp/ring_buffer.hpp"
#include "sample_source.hpp"

#include <uhd/usrp/multi_usrp.hpp>

namespace famidec {

// Native UHD input source. UHD receives sc16 samples on a worker thread,
// converts them to interleaved signed 8-bit IQ, and exposes the same pull
// interface used by HackRfSource and FileSource.
class UhdSource : public ISampleSource {
public:
    explicit UhdSource(const Config& cfg);
    ~UhdSource() override;

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

    bool set_center_freq(double center_hz) override;
    bool restart() override;
    bool set_gains(int lna, int vga) override;
    bool set_amp(bool on) override;

    const std::string& error() const override { return error_; }
    const std::string& device_info() const { return device_info_; }

private:
    void rx_loop();

    const Config& cfg_;
    uhd::usrp::multi_usrp::sptr usrp_;
    uhd::rx_streamer::sptr rx_streamer_;
    SpscRing ring_;
    SampleFormat format_ = SampleFormat::CS16;

    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> clipped_{0};
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex control_mutex_;

    double uhd_gain_ = 0.0;
    std::string device_info_;
    std::string error_;
};

}  // namespace famidec
