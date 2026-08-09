#include "soapy_source.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>

namespace famidec {

namespace {
constexpr size_t kRingBytes = 1u << 25;  // 32 MiB ~ 1.6 s at 10 MSPS
constexpr size_t kReadBatchSamples = 1 << 14;  // 16384 complex samples per read (~1.6ms at 10 MSPS)
constexpr const char* kSampleFormat = SOAPY_SDR_CS16;  // Complex 16-bit (native B210 format)
constexpr long kReadTimeoutUs = 100000;  // 100ms timeout in microseconds
}  // namespace

SoapySource::SoapySource(const Config& cfg, const std::string& device_args)
    : cfg_(cfg), device_args_(device_args), ring_(kRingBytes) {}

SoapySource::~SoapySource() {
    stop();
}

std::vector<std::string> SoapySource::enumerate_devices() {
    std::vector<std::string> devices;
    try {
        auto args_list = SoapySDR::Device::enumerate();
        for (const auto& args : args_list) {
            // Build a human-readable string from the Kwargs map
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
        // Return empty on error
    }
    return devices;
}

bool SoapySource::configure_gains() {
    if (!device_) return false;

    bool configured = false;
    
    // Get list of available gain names
    try {
        auto gain_names = device_->listGains(SOAPY_SDR_RX, 0);
        
        // Try to set middle values for common gain names
        for (const auto& name : gain_names) {
            try {
                auto range = device_->getGainRange(SOAPY_SDR_RX, 0, name);
                double middle = (range.minimum() + range.maximum()) / 2.0;
                device_->setGain(SOAPY_SDR_RX, 0, name, middle);
                
                // Map to our internal values based on name
                if (name.find("LNA") != std::string::npos || 
                    name.find("RF") != std::string::npos) {
                    // Map 0-32 dB to 0-40
                    lna_ = static_cast<int>((middle / 32.0) * 40.0);
                } else if (name.find("VGA") != std::string::npos) {
                    // Map 0-47 dB to 0-62
                    vga_ = static_cast<int>((middle / 47.0) * 62.0);
                }
                
                configured = true;
            } catch (...) {
                // Skip gains that fail
            }
        }
    } catch (...) {
        // No gains available
    }
    
    return configured;
}

bool SoapySource::start() {
    std::lock_guard<std::mutex> lk(mu_);

    // Open device
    try {
        if (!device_args_.empty()) {
            device_ = SoapySDR::Device::make(device_args_);
        } else {
            device_ = SoapySDR::Device::make();
        }
    } catch (const std::exception& e) {
        error_ = std::string("SoapySDR::make: ") + e.what();
        return false;
    }

    if (!device_) {
        error_ = "Failed to open SoapySDR device";
        return false;
    }

    // Get device info
    try {
        auto hw = device_->getHardwareInfo();
        device_info_ = hw.count("driver") ? hw.at("driver") : "SoapySDR";
        if (hw.count("version")) {
            device_info_ += " v" + hw.at("version");
        }
        if (hw.count("product")) {
            device_info_ += " (" + hw.at("product") + ")";
        }
    } catch (...) {
        device_info_ = "SoapySDR device";
    }

    // Set sample rate
    try {
        device_->setSampleRate(SOAPY_SDR_RX, 0, cfg_.sample_rate);
    } catch (const std::exception& e) {
        error_ = std::string("setSampleRate: ") + e.what();
        return false;
    }

    // Set frequency (SoapySDR uses MHz)
    try {
        device_->setFrequency(SOAPY_SDR_RX, 0, cfg_.center_hz() / 1e6, {});
    } catch (const std::exception& e) {
        error_ = std::string("setFrequency: ") + e.what();
        return false;
    }

    // Configure gains
    configure_gains();

    // Setup stream
    try {
        stream_ = device_->setupStream(
            SOAPY_SDR_RX, kSampleFormat, {0}, {});
        if (!stream_) {
            error_ = "Failed to setup SoapySDR stream";
            return false;
        }
    } catch (const std::exception& e) {
        error_ = std::string("setupStream: ") + e.what();
        return false;
    }

    // Activate stream
    try {
        device_->activateStream(stream_, 0, 0, 0);
    } catch (const std::exception& e) {
        error_ = std::string("activateStream: ") + e.what();
        return false;
    }

    running_.store(true, std::memory_order_relaxed);
    return true;
}

void SoapySource::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!device_) return;
    
    running_.store(false, std::memory_order_relaxed);
    
    try {
        if (stream_) {
            device_->deactivateStream(stream_, 0, 0);
        }
    } catch (...) {
        // Ignore errors during shutdown
    }

    // Cleanup
    if (stream_) {
        try {
            device_->closeStream(stream_);
        } catch (...) {
            // Ignore
        }
        stream_ = nullptr;
    }
    if (device_) {
        delete device_;
        device_ = nullptr;
    }
}

void SoapySource::pause() {
    // Just flip the running flag without touching the device.
    // The DSP thread will see running_ == false and return from read(),
    // allowing readStream() to timeout gracefully.
    running_.store(false, std::memory_order_release);
}

void SoapySource::resume() {
    // Restart the stream without recreating the device.
    // This is safe to call after pause() even if a readStream() call
    // is blocked.
    std::lock_guard<std::mutex> lk(mu_);
    running_.store(true, std::memory_order_relaxed);
    
    if (device_ && stream_) {
        try {
            // Re-activate the stream so readStream() will unblock
            device_->deactivateStream(stream_, 0, 0);
            device_->activateStream(stream_, 0, 0, 0);
        } catch (...) {
            // Ignore
        }
    }
}

size_t SoapySource::read(uint8_t* buf, size_t len) {
    if (!running_.load(std::memory_order_relaxed)) return 0;

    size_t got = 0;

    // Pre-allocate buffer outside loop to avoid repeated allocation
    std::vector<int16_t> samples(kRingBytes / 2);

    while (got < len && running_.load(std::memory_order_relaxed)) {
        // Try to get data from ring buffer first
        size_t n = ring_.pop(buf + got, len - got);
        got += n;
        if (n > 0) continue;

        // Ring buffer empty, read from device using blocking I/O
        // SoapySDR CS16 format: complex 16-bit = {I0,Q0,I1,Q1,...} as int16_t pairs
        // readStream expects void* const* (array of buffer pointers)
        void* buffers[] = {samples.data()};
        size_t nsamples = samples.size();  // in/out: requested / actual count
        int status = 0;
        long long timeNs = 0;

        try {
            int actual = device_->readStream(
                stream_,
                buffers,
                nsamples,
                status,
                timeNs,
                100000);  // 100ms timeout in microseconds

            if (actual < 0) {
                // Error: sleep briefly and retry
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (actual == 0) {
                // No data available within timeout: sleep to avoid busy loop
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            {
                // Convert CS16 (16-bit) to interleaved int8_t for fpvdec pipeline
                // CS16: each complex sample = I16,Q16 (2 int16_t = 4 bytes)
                static std::vector<uint8_t> converted(kRingBytes);
                size_t out_idx = 0;
                
                // Clip detection on 16-bit values
                uint64_t clips = 0;
                for (int i = 0; i < actual; i++) {
                    int16_t i_sample = samples[i * 2];
                    int16_t q_sample = samples[i * 2 + 1];
                    
                    // Convert 16-bit to 8-bit by taking upper byte (scale down)
                    int8_t i8 = static_cast<int8_t>(i_sample >> 8);
                    int8_t q8 = static_cast<int8_t>(q_sample >> 8);
                    
                    converted[out_idx++] = static_cast<uint8_t>(i8);
                    converted[out_idx++] = static_cast<uint8_t>(q8);
                    
                    // Clip detection: values near ±32767 are clipped
                    if (i_sample < -32000 || i_sample > 32000) clips++;
                    if (q_sample < -32000 || q_sample > 32000) clips++;
                }
                
                // Count raw bytes from device (4 bytes per complex sample in CS16)
                total_.fetch_add(actual * 4, std::memory_order_relaxed);
                
                if (clips) clipped_.fetch_add(clips, std::memory_order_relaxed);
                
                // Push converted 8-bit interleaved IQ data to ring buffer
                if (!ring_.push(converted.data(), out_idx)) {
                    dropped_.fetch_add(out_idx, std::memory_order_relaxed);
                }
            }
        } catch (const std::exception& e) {
            // If read fails, sleep and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    return got;
}

float SoapySource::ring_fill() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<float>(ring_.readable()) /
           static_cast<float>(ring_.capacity());
}

bool SoapySource::set_gains(int lna, int vga) {
    if (!device_) return false;
    
    std::lock_guard<std::mutex> lk(mu_);
    
    lna = std::clamp(lna, 0, 40);
    vga = std::clamp(vga, 0, 62);
    
    lna_ = lna;
    vga_ = vga;
    
    // Map to dB and apply to common gain names
    // B220mini: LNA (0-32 dB), VGA (0-47 dB), RF (25-80 dB)
    try {
        auto gain_names = device_->listGains(SOAPY_SDR_RX, 0);
        
        for (const auto& name : gain_names) {
            try {
                auto range = device_->getGainRange(SOAPY_SDR_RX, 0, name);
                double db;
                
                if (name.find("LNA") != std::string::npos || 
                    name.find("RF") != std::string::npos) {
                    // Map 0-40 to range
                    db = lna * (range.maximum() - range.minimum()) / 40.0 + range.minimum();
                } else if (name.find("VGA") != std::string::npos) {
                    // Map 0-62 to range
                    db = vga * (range.maximum() - range.minimum()) / 62.0 + range.minimum();
                } else {
                    // Unknown gain name, skip
                    continue;
                }
                
                device_->setGain(SOAPY_SDR_RX, 0, name, db);
            } catch (...) {
                // Skip gains that fail
            }
        }
    } catch (...) {
        // No gain names available
    }
    
    return true;
}

bool SoapySource::set_center_freq(double center_hz) {
    if (!device_) return false;
    
    std::lock_guard<std::mutex> lk(mu_);
    
    try {
        device_->setFrequency(SOAPY_SDR_RX, 0, center_hz / 1e6, {});
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace famidec
