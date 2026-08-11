#include "uhd_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <cstring>
#include <vector>

namespace famidec {
namespace {
constexpr size_t kRingBytes = 1u << 25;       // 32 MiB
constexpr long kRecvTimeout = 0.1;             // seconds
constexpr double kSc16Clip = 32000.0;
}

UhdSource::UhdSource(const Config& cfg)
    : cfg_(cfg), ring_(kRingBytes), format_(cfg.sample_bits == 16 ? SampleFormat::CS16 : SampleFormat::CS8), uhd_gain_(cfg.uhd_gain_db) {}

UhdSource::~UhdSource() { stop(); }

bool UhdSource::start() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (running_.load(std::memory_order_acquire)) return true;

    try {
        usrp_ = uhd::usrp::multi_usrp::make(cfg_.uhd_device_args);
        usrp_->set_rx_rate(cfg_.sample_rate, 0);
        usrp_->set_rx_freq(cfg_.center_hz(), 0);
        if (uhd_gain_ >= 0.0) usrp_->set_rx_gain(uhd_gain_, 0);
        if (!cfg_.uhd_antenna.empty()) usrp_->set_rx_antenna(cfg_.uhd_antenna, 0);

        uhd::stream_args_t stream_args("sc16", "sc16");
        stream_args.channels = {0};
        rx_streamer_ = usrp_->get_rx_stream(stream_args);

        device_info_ = usrp_->get_pp_string();
        std::printf("UHD: %s, rate %.3f MSPS, gain %.1f dB\n",
                    device_info_.c_str(), cfg_.sample_rate / 1e6, uhd_gain_);
    } catch (const std::exception& e) {
        error_ = std::string("UHD configure: ") + e.what();
        rx_streamer_.reset();
        usrp_.reset();
        return false;
    }

    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&UhdSource::rx_loop, this);
    return true;
}

void UhdSource::stop() {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        running_.store(false, std::memory_order_release);
        if (rx_streamer_) {
            try {
                uhd::stream_cmd_t stop_cmd(
                    uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
                rx_streamer_->issue_stream_cmd(stop_cmd);
            } catch (...) {
            }
        }
    }
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(control_mutex_);
    rx_streamer_.reset();
    usrp_.reset();
}

void UhdSource::pause() {
    running_.store(false, std::memory_order_release);
}

void UhdSource::resume() {
    if (!rx_streamer_) return;
    running_.store(true, std::memory_order_release);
}

void UhdSource::rx_loop() {
    const size_t spb = rx_streamer_->get_max_num_samps();
    std::vector<int16_t> sc16(spb * 2);
    std::vector<uint8_t> sc8(spb * 2);
    std::vector<uint8_t> sc16_bytes(spb * 4);
    uhd::rx_metadata_t md;

    try {
        uhd::stream_cmd_t start_cmd(
            uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        start_cmd.stream_now = true;
        rx_streamer_->issue_stream_cmd(start_cmd);

        while (running_.load(std::memory_order_acquire)) {
            size_t n = rx_streamer_->recv(sc16.data(), spb, md, kRecvTimeout);
            if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) continue;
            if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE || n == 0) {
                if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
                    continue;
                if (running_.load(std::memory_order_acquire))
                    error_ = std::string("UHD receive: ") + md.strerror();
                break;
            }

            uint64_t clips = 0;
            for (size_t i = 0; i < n * 2; ++i) {
                const int16_t sample = sc16[i];
                if (std::abs(static_cast<int>(sample)) >= kSc16Clip) ++clips;
                sc8[i] = static_cast<uint8_t>(static_cast<int8_t>(sample >> 8));
            }
            total_.fetch_add(n * 4, std::memory_order_relaxed);
            if (clips) clipped_.fetch_add(clips, std::memory_order_relaxed);
            if (format_ == SampleFormat::CS16) {
                std::memcpy(sc16_bytes.data(), sc16.data(), n * 4);
                if (!ring_.push(sc16_bytes.data(), n * 4))
                    dropped_.fetch_add(n * 4, std::memory_order_relaxed);
            } else if (!ring_.push(sc8.data(), n * 2)) {
                dropped_.fetch_add(n * 2, std::memory_order_relaxed);
            }
        }

        if (rx_streamer_) {
            uhd::stream_cmd_t stop_cmd(
                uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
            rx_streamer_->issue_stream_cmd(stop_cmd);
        }
    } catch (const std::exception& e) {
        if (running_.load(std::memory_order_acquire))
            error_ = std::string("UHD receive: ") + e.what();
    }
    running_.store(false, std::memory_order_release);
}

size_t UhdSource::read(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len && running_.load(std::memory_order_acquire)) {
        const size_t n = ring_.pop(buf + got, len - got);
        got += n;
        if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return got;
}

float UhdSource::ring_fill() const {
    return static_cast<float>(ring_.readable()) /
           static_cast<float>(ring_.capacity());
}

bool UhdSource::set_center_freq(double center_hz) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!usrp_) return false;
    try {
        usrp_->set_rx_freq(center_hz, 0);
        return true;
    } catch (...) {
        return false;
    }
}

bool UhdSource::set_gains(int, int) {
    // UHD has device-specific gain stages; the generic fpvdec LNA/VGA values
    // do not map reliably. Use --uhd-gain for the aggregate UHD RX gain.
    return false;
}

bool UhdSource::set_amp(bool) {
    // UHD devices generally do not expose a HackRF-style RF amp switch.
    return false;
}

}  // namespace famidec
