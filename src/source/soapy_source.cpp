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
constexpr const char* kSampleFormat = SOAPY_SDR_CS8;  // Complex 8-bit
constexpr int kReadTimeoutMs = 1000;  // 1 second timeout
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
    {
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

size_t SoapySource::read(uint8_t* buf, size_t len) {
    if (!running_.load(std::memory_order_relaxed)) return 0;

    size_t got = 0;
    
    while (got < len && running_.load(std::memory_order_relaxed)) {
        // Try to get data from ring buffer first
        size_t n = ring_.pop(buf + got, len - got);
        got += n;
        if (n > 0) continue;
        
        // Ring buffer empty, read from device using blocking I/O
        // SoapySDR CS8 format: complex 8-bit = {I0, Q0, I1, Q1, ...}
        // readStream expects void* const* (array of buffer pointers)
        std::vector<int8_t> samples(kRingBytes);
        size_t nsamples = samples.size();
        void* buffers[] = {samples.data()};
        
        int status = 0;
        long long timeNs = 0;
        
        try {
            size_t actual = device_->readStream(
                stream_,
                buffers,
                nsamples,
                status,
                timeNs,
                kReadTimeoutMs);

            if (actual > 0) {
                total_.fetch_add(actual * 2, std::memory_order_relaxed);
                
                // Clip detection
                uint64_t clips = 0;
                int mx = 0;
                for (size_t i = 0; i < actual * 2; i += 64) {
                    int v = samples[i];
                    if (v < 0) v = -v;
                    if (v > mx) mx = v;
                    if (v >= 127) ++clips;
                }
                if (clips) clipped_.fetch_add(clips, std::memory_order_relaxed);
                
                // Push to ring buffer (samples is already I0,Q0,I1,Q1...)
                if (!ring_.push(reinterpret_cast<const uint8_t*>(samples.data()), actual * 2)) {
                    dropped_.fetch_add(actual * 2, std::memory_order_relaxed);
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
