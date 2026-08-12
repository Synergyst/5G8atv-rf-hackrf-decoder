#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include <libhackrf/hackrf.h>

#include "../config.hpp"
#include "../dsp/ring_buffer.hpp"
#include "sample_source.hpp"

namespace famidec {

class HackRfSource : public ISampleSource {
public:
    explicit HackRfSource(const Config& cfg);
    ~HackRfSource() override;

    bool start() override;
    void stop() override;
    void pause() override;  // set running_ = false to unblock read()
    void resume() override;
    size_t read(uint8_t* buf, size_t len) override;

    uint64_t dropped_bytes() const override {
        return dropped_.load(std::memory_order_relaxed);
    }
    uint64_t total_bytes() const override {
        return total_.load(std::memory_order_relaxed);
    }
    uint64_t buffered_bytes() const override { return ring_.readable(); }
    uint64_t clipped_samples() const override {
        return clipped_.load(std::memory_order_relaxed);
    }
    float ring_fill() const override;

    bool set_gains(int lna, int vga);
    bool set_center_freq(double center_hz);
    bool set_amp(bool on);
    bool restart() override;
    // CLKIN: check if external clock is locked (0 = no, 1 = yes).
    bool check_clkin() const;
    // CLKOUT: enable or disable 10 MHz clock output.
    bool set_clkout(bool on);
    int lna() const { return lna_; }
    int vga() const { return vga_; }

    // Peak |sample| (0..127) observed since the last call; resets on read.
    // Fed by the same strided scan as the clip counter, for the gain AGC.
    int take_peak() { return peak_.exchange(0, std::memory_order_relaxed); }

    const std::string& error() const override { return error_; }

private:
    static int rx_callback(hackrf_transfer* transfer);
    void cleanup_device();
    void set_error(const char* operation, int result);
    void append_error(const char* operation, int result);

    const Config& cfg_;
    hackrf_device* dev_ = nullptr;
    SpscRing ring_;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> clipped_{0};
    std::atomic<int> peak_{0};
    std::atomic<bool> running_{false};
    int lna_, vga_;
    std::string error_;

    // All libhackrf lifecycle calls are serialized. running_ is published to
    // the USB callback before start and cleared before stop_rx/close.
    mutable std::mutex lifecycle_mutex_;
    bool library_initialized_ = false;
    bool streaming_ = false;
};

}  // namespace famidec
