#include "hackrf_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>

namespace famidec {

namespace {
constexpr size_t kRingBytes = 1u << 25;  // 32 MiB ~ 1.6 s at 10 MSPS

const char* error_name(int result) {
    const char* name = hackrf_error_name(static_cast<hackrf_error>(result));
    return name ? name : "unknown error";
}

bool expected_stop_result(int result) {
    return result == HACKRF_SUCCESS ||
           result == HACKRF_ERROR_STREAMING_STOPPED ||
           result == HACKRF_ERROR_STREAMING_EXIT_CALLED;
}

}  // namespace

HackRfSource::HackRfSource(const Config& cfg)
    : cfg_(cfg), ring_(kRingBytes), lna_(cfg.lna_gain), vga_(cfg.vga_gain) {}

HackRfSource::~HackRfSource() { stop(); }

void HackRfSource::set_error(const char* operation, int result) {
    error_ = std::string(operation) + ": " + error_name(result) +
             " (" + std::to_string(result) + ")";
}

void HackRfSource::append_error(const char* operation, int result) {
    const std::string detail = std::string(operation) + ": " +
                               error_name(result) + " (" +
                               std::to_string(result) + ")";
    if (error_.empty())
        error_ = detail;
    else
        error_ += "; " + detail;
}

void HackRfSource::cleanup_device() {
    // The caller holds lifecycle_mutex_. Clear running_ before touching the
    // stream so an in-flight callback will stop accepting new data. libhackrf
    // waits for callbacks to finish in hackrf_stop_rx()/hackrf_close().
    running_.store(false, std::memory_order_release);

    if (streaming_ && dev_) {
        const int r = hackrf_stop_rx(dev_);
        if (!expected_stop_result(r)) append_error("hackrf_stop_rx", r);
        streaming_ = false;
    }

    if (dev_) {
        hackrf_device* device = dev_;
        dev_ = nullptr;
        const int r = hackrf_close(device);
        if (r != HACKRF_SUCCESS) append_error("hackrf_close", r);
    }

    if (library_initialized_) {
        const int r = hackrf_exit();
        if (r != HACKRF_SUCCESS) append_error("hackrf_exit", r);
        library_initialized_ = false;
    }
}

int HackRfSource::rx_callback(hackrf_transfer* transfer) {
    if (!transfer || !transfer->rx_ctx) return -1;
    auto* self = static_cast<HackRfSource*>(transfer->rx_ctx);
    if (!self->running_.load(std::memory_order_acquire)) return -1;
    if (!transfer->buffer || transfer->valid_length < 0 ||
        transfer->valid_length > transfer->buffer_length)
        return -1;

    const uint8_t* data = transfer->buffer;
    const size_t len = static_cast<size_t>(transfer->valid_length);
    // Cheap clip + peak detection on a stride to keep the USB thread light.
    uint64_t clips = 0;
    int mx = 0;
    for (size_t i = 0; i < len; i += 64) {
        int v = static_cast<int8_t>(data[i]);
        if (v < 0) v = -v;
        if (v > mx) mx = v;
        if (v >= 127) ++clips;
    }
    if (clips) self->clipped_.fetch_add(clips, std::memory_order_relaxed);
    int cur = self->peak_.load(std::memory_order_relaxed);
    while (mx > cur &&
           !self->peak_.compare_exchange_weak(cur, mx,
                                              std::memory_order_relaxed)) {
    }

    // Stop can race with a callback already in progress. Do not account or
    // enqueue a transfer after the stop request has been published.
    if (!self->running_.load(std::memory_order_acquire)) return -1;

    // total_ is transport input, while dropped_ is the subset that could not
    // be handed to the DSP ring. Keeping both byte counters in the same unit
    // makes loss rates and diagnostics meaningful.
    self->total_.fetch_add(len, std::memory_order_relaxed);
    if (!self->ring_.push(data, len))
        self->dropped_.fetch_add(len, std::memory_order_relaxed);
    return 0;
}

bool HackRfSource::start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) return true;

    // Recover from a partially initialized previous attempt before retrying.
    if (dev_ || library_initialized_ || streaming_) cleanup_device();
    error_.clear();

    int r = hackrf_init();
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_init", r);
        return false;
    }
    library_initialized_ = true;

    dev_ = nullptr;
    r = hackrf_open(&dev_);
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_open", r);
        cleanup_device();
        return false;
    }

    if (!std::isfinite(cfg_.sample_rate) || cfg_.sample_rate <= 0.0 ||
        cfg_.sample_rate > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        error_ = "sample rate is outside libhackrf's supported numeric range";
        cleanup_device();
        return false;
    }
    const auto sample_rate = static_cast<uint32_t>(cfg_.sample_rate);
    const double filter_rate_hz = cfg_.sample_rate * 0.9;
    if (!std::isfinite(filter_rate_hz) || filter_rate_hz <= 0.0 ||
        filter_rate_hz > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        error_ = "baseband filter rate is outside libhackrf's supported numeric range";
        cleanup_device();
        return false;
    }
    if (!std::isfinite(cfg_.center_hz()) || cfg_.center_hz() < 0.0 ||
        cfg_.center_hz() > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        error_ = "center frequency is outside libhackrf's supported numeric range";
        cleanup_device();
        return false;
    }

    r = hackrf_set_sample_rate(dev_, sample_rate);
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_set_sample_rate", r);
        cleanup_device();
        return false;
    }
    // 0.9*rate, not the usual 0.75: at 10 MSPS the analog filter must stay
    // open through the chroma upper sideband (+-4.6 MHz), or color washes
    // out just like a too-narrow digital channel filter.
    r = hackrf_set_baseband_filter_bandwidth(
        dev_, hackrf_compute_baseband_filter_bw(
                  static_cast<uint32_t>(filter_rate_hz)));
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_set_baseband_filter_bandwidth", r);
        cleanup_device();
        return false;
    }
    r = hackrf_set_freq(dev_, static_cast<uint64_t>(cfg_.center_hz()));
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_set_freq", r);
        cleanup_device();
        return false;
    }
    r = hackrf_set_amp_enable(dev_, cfg_.amp ? 1 : 0);
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_set_amp_enable", r);
        cleanup_device();
        return false;
    }
    r = hackrf_set_lna_gain(dev_, static_cast<uint32_t>(lna_));
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_set_lna_gain", r);
        cleanup_device();
        return false;
    }
    r = hackrf_set_vga_gain(dev_, static_cast<uint32_t>(vga_));
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_set_vga_gain", r);
        cleanup_device();
        return false;
    }
    // Always enable CLKOUT by default (10 MHz clock output for GPSDO).
    if (cfg_.clkout) {
        r = hackrf_set_clkout_enable(dev_, 1);
        if (r != HACKRF_SUCCESS) {
            set_error("hackrf_set_clkout_enable", r);
            cleanup_device();
            return false;
        }
    }

    // Publish running before start_rx: the first callback may be dispatched
    // immediately by libhackrf after the stream is started. Mark the cleanup
    // path as streaming before the call so a failed start is still stopped.
    running_.store(true, std::memory_order_release);
    streaming_ = true;
    r = hackrf_start_rx(dev_, rx_callback, this);
    if (r != HACKRF_SUCCESS) {
        running_.store(false, std::memory_order_release);
        set_error("hackrf_start_rx", r);
        cleanup_device();
        return false;
    }
    error_.clear();
    return true;
}

void HackRfSource::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!dev_ && !library_initialized_ && !streaming_) {
        running_.store(false, std::memory_order_release);
        return;
    }
    cleanup_device();
}

void HackRfSource::pause() {
    // Stop the libhackrf stream as well as marking the source paused. Merely
    // flipping running_ can leave the USB callback stopped while resume()
    // only changes the flag, causing the next DSP instance to see EOF.
    stop();
}

void HackRfSource::resume() {
    if (!start() && error_.empty()) error_ = "HackRF resume failed";
}

size_t HackRfSource::read(uint8_t* buf, size_t len) {
    if (!buf || len == 0) return 0;
    size_t got = 0;
    while (got < len && running_.load(std::memory_order_acquire)) {
        size_t n = ring_.pop(buf + got, len - got);
        got += n;
        if (n == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return got;
}

float HackRfSource::ring_fill() const {
    return static_cast<float>(ring_.readable()) /
           static_cast<float>(ring_.capacity());
}

bool HackRfSource::restart() {
    stop();
    return start();
}

bool HackRfSource::set_center_freq(double center_hz) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!dev_) {
        error_ = "hackrf_set_freq: device is not open";
        return false;
    }
    if (!std::isfinite(center_hz) || center_hz < 0.0 ||
        center_hz > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        error_ = "hackrf_set_freq: invalid center frequency";
        return false;
    }
    const int r = hackrf_set_freq(dev_, static_cast<uint64_t>(center_hz));
    if (r != HACKRF_SUCCESS) set_error("hackrf_set_freq", r);
    return r == HACKRF_SUCCESS;
}

bool HackRfSource::set_amp(bool on) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!dev_) {
        error_ = "hackrf_set_amp_enable: device is not open";
        return false;
    }
    const int r = hackrf_set_amp_enable(dev_, on ? 1 : 0);
    if (r != HACKRF_SUCCESS) set_error("hackrf_set_amp_enable", r);
    return r == HACKRF_SUCCESS;
}

bool HackRfSource::set_gains(int lna, int vga) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const int new_lna = std::clamp((lna / 8) * 8, 0, 40);
    const int new_vga = std::clamp((vga / 2) * 2, 0, 62);
    if (!dev_) {
        error_ = "hackrf_set_lna_gain: device is not open";
        return false;
    }
    int r = hackrf_set_lna_gain(dev_, static_cast<uint32_t>(new_lna));
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_set_lna_gain", r);
        return false;
    }
    r = hackrf_set_vga_gain(dev_, static_cast<uint32_t>(new_vga));
    if (r != HACKRF_SUCCESS) {
        set_error("hackrf_set_vga_gain", r);
        return false;
    }
    lna_ = new_lna;
    vga_ = new_vga;
    return true;
}

bool HackRfSource::check_clkin() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!dev_) return false;
    uint8_t status = 0;
    const int r = hackrf_get_clkin_status(dev_, &status);
    return (r == HACKRF_SUCCESS && status != 0);
}

bool HackRfSource::set_clkout(bool on) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!dev_) {
        error_ = "hackrf_set_clkout_enable: device is not open";
        return false;
    }
    const int r = hackrf_set_clkout_enable(dev_, on ? 1 : 0);
    if (r != HACKRF_SUCCESS) set_error("hackrf_set_clkout_enable", r);
    return r == HACKRF_SUCCESS;
}

}  // namespace famidec
