#include "soapy_source.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace famidec {
namespace {
constexpr size_t kRingBytes = 1u << 25;
constexpr size_t kReadBatchSamples = 1u << 14;
constexpr long kReadTimeoutUs = 100000;
constexpr int16_t kClipThreshold = 32000;
}

SoapySource::SoapySource(const Config& cfg, const std::string& device_args)
    : cfg_(cfg), device_args_(device_args), ring_(kRingBytes),
      format_(cfg.sample_bits == 16 ? SampleFormat::CS16 : SampleFormat::CS8),
      lna_(cfg.lna_gain), vga_(cfg.vga_gain) {}

SoapySource::~SoapySource() { stop(); }

std::vector<std::string> SoapySource::enumerate_devices() {
    std::vector<std::string> devices;
    try {
        for (const auto& args : SoapySDR::Device::enumerate()) {
            std::ostringstream oss;
            bool first = true;
            for (const auto& kv : args) {
                if (!first) oss << ", ";
                oss << kv.first << "=" << kv.second;
                first = false;
            }
            devices.push_back(oss.str());
        }
    } catch (...) {
    }
    return devices;
}

bool SoapySource::configure_gains_locked() {
    if (!device_) return false;
    try {
        auto names = device_->listGains(SOAPY_SDR_RX, 0);
        if (names.empty()) {
            device_->setGain(SOAPY_SDR_RX, 0, 0.0);
            return true;
        }
        bool ok = false;
        for (const auto& name : names) {
            try {
                auto range = device_->getGainRange(SOAPY_SDR_RX, 0, name);
                double value = (range.minimum() + range.maximum()) * 0.5;
                device_->setGain(SOAPY_SDR_RX, 0, name, value);
                ok = true;
            } catch (...) {
            }
        }
        return ok;
    } catch (...) {
        return false;
    }
}

bool SoapySource::set_gains_locked(int lna, int vga) {
    if (!device_) return false;
    lna_ = std::clamp((lna / 8) * 8, 0, 40);
    vga_ = std::clamp((vga / 2) * 2, 0, 62);
    bool ok = false;
    try {
        auto names = device_->listGains(SOAPY_SDR_RX, 0);
        for (const auto& name : names) {
            try {
                auto range = device_->getGainRange(SOAPY_SDR_RX, 0, name);
                double fraction = 0.5;
                if (name.find("LNA") != std::string::npos ||
                    name.find("RF") != std::string::npos) {
                    fraction = static_cast<double>(lna_) / 40.0;
                } else if (name.find("VGA") != std::string::npos) {
                    fraction = static_cast<double>(vga_) / 62.0;
                }
                const double value = range.minimum() +
                    std::clamp(fraction, 0.0, 1.0) *
                    (range.maximum() - range.minimum());
                device_->setGain(SOAPY_SDR_RX, 0, name, value);
                ok = true;
            } catch (...) {
            }
        }
    } catch (...) {
    }
    return ok;
}

bool SoapySource::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (running_.load(std::memory_order_acquire)) return true;

    try {
        device_ = device_args_.empty() ? SoapySDR::Device::make()
                                        : SoapySDR::Device::make(device_args_);
        if (!device_) {
            error_ = "SoapySDR::make returned no device";
            return false;
        }
        auto hw = device_->getHardwareInfo();
        device_info_ = hw.count("driver") ? hw.at("driver") : "SoapySDR";
        if (hw.count("product")) device_info_ += " (" + hw.at("product") + ")";

        device_->setSampleRate(SOAPY_SDR_RX, 0, cfg_.sample_rate);
        device_->setFrequency(SOAPY_SDR_RX, 0, cfg_.center_hz(), {});
        configure_gains_locked();
        set_gains_locked(cfg_.lna_gain, cfg_.vga_gain);

        stream_ = device_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {0}, {});
        if (!stream_) throw std::runtime_error("setupStream returned null");
        device_->activateStream(stream_, 0, 0, 0);
    } catch (const std::exception& e) {
        error_ = std::string("SoapySDR start: ") + e.what();
        if (device_ && stream_) {
            try { device_->closeStream(stream_); } catch (...) {}
        }
        stream_ = nullptr;
        if (device_) SoapySDR::Device::unmake(device_);
        device_ = nullptr;
        return false;
    }

    error_.clear();
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&SoapySource::rx_loop, this);
    return true;
}

void SoapySource::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_.store(false, std::memory_order_release);
        if (device_ && stream_) {
            try { device_->deactivateStream(stream_, 0, 0); } catch (...) {}
        }
    }
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(mu_);
    if (device_ && stream_) {
        try { device_->closeStream(stream_); } catch (...) {}
    }
    stream_ = nullptr;
    if (device_) SoapySDR::Device::unmake(device_);
    device_ = nullptr;
}

void SoapySource::pause() {
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (device_ && stream_) {
            try { device_->deactivateStream(stream_, 0, 0); } catch (...) {}
        }
    }
    if (worker_.joinable()) worker_.join();
}

void SoapySource::resume() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!device_ || !stream_) return;
    try {
        device_->activateStream(stream_, 0, 0, 0);
        running_.store(true, std::memory_order_release);
        worker_ = std::thread(&SoapySource::rx_loop, this);
    } catch (const std::exception& e) {
        error_ = std::string("SoapySDR resume: ") + e.what();
    }
}

void SoapySource::rx_loop() {
    std::vector<int16_t> samples(kReadBatchSamples * 2);
    std::vector<uint8_t> converted(kReadBatchSamples * 2);
    std::vector<uint8_t> raw16(kReadBatchSamples * 4);
    while (running_.load(std::memory_order_acquire)) {
        void* buffers[] = {samples.data()};
        size_t requested = kReadBatchSamples;
        int flags = 0;
        long long time_ns = 0;
        int actual = 0;
        try {
            actual = device_->readStream(stream_, buffers, requested, flags,
                                         time_ns, kReadTimeoutUs);
        } catch (const std::exception& e) {
            if (running_.load(std::memory_order_acquire))
                error_ = std::string("SoapySDR readStream: ") + e.what();
            break;
        }
        if (actual == SOAPY_SDR_TIMEOUT) continue;
        if (actual < 0) {
            if (actual == SOAPY_SDR_OVERFLOW) continue;
            if (running_.load(std::memory_order_acquire))
                error_ = "SoapySDR readStream error " + std::to_string(actual);
            break;
        }
        if (actual == 0) continue;

        uint64_t clips = 0;
        for (int i = 0; i < actual * 2; ++i) {
            const int16_t sample = samples[static_cast<size_t>(i)];
            if (std::abs(static_cast<int>(sample)) >= kClipThreshold) ++clips;
            converted[static_cast<size_t>(i)] =
                static_cast<uint8_t>(static_cast<int8_t>(sample >> 8));
            std::memcpy(raw16.data() + static_cast<size_t>(i) * sizeof(int16_t),
                        &sample, sizeof(sample));
        }
        total_.fetch_add(static_cast<uint64_t>(actual) * 4,
                         std::memory_order_relaxed);
        if (clips) clipped_.fetch_add(clips, std::memory_order_relaxed);
        const size_t bytes = static_cast<size_t>(actual) * (format_ == SampleFormat::CS16 ? 4 : 2);
        const uint8_t* output = format_ == SampleFormat::CS16 ? raw16.data() : converted.data();
        if (!ring_.push(output, bytes))
            dropped_.fetch_add(bytes, std::memory_order_relaxed);
    }
    running_.store(false, std::memory_order_release);
}

size_t SoapySource::read(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len && running_.load(std::memory_order_acquire)) {
        const size_t n = ring_.pop(buf + got, len - got);
        got += n;
        if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return got;
}

float SoapySource::ring_fill() const {
    return static_cast<float>(ring_.readable()) /
           static_cast<float>(ring_.capacity());
}

bool SoapySource::set_gains(int lna, int vga) {
    std::lock_guard<std::mutex> lock(mu_);
    return set_gains_locked(lna, vga);
}

bool SoapySource::set_center_freq(double center_hz) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!device_) return false;
    try {
        device_->setFrequency(SOAPY_SDR_RX, 0, center_hz, {});
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace famidec
