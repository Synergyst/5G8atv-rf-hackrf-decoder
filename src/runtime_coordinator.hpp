#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include "config.hpp"
#include "dsp/ntsc_decoder.hpp"
#include "runtime_control.hpp"
#include "runtime_lifecycle.hpp"
#include "source/sample_source.hpp"

namespace famidec {

class IRawRecorder {
public:
    virtual ~IRawRecorder() = default;
    virtual void write(const uint8_t* data, size_t size) = 0;
};

class RuntimeCoordinator {
public:
    RuntimeCoordinator(Config& cfg, RuntimeControl& control,
                       RuntimeLifecycle& lifecycle, std::atomic<bool>& running);
    ~RuntimeCoordinator();
    RuntimeCoordinator(const RuntimeCoordinator&) = delete;
    RuntimeCoordinator& operator=(const RuntimeCoordinator&) = delete;

    bool create_source(bool pace_file);
    ISampleSource* source() const { return source_.get(); }
    bool start_source();
    void stop_source();
    bool start_dsp(NtscDecoder* decoder, IRawRecorder* recorder,
                   std::atomic<float>* mean_raw, ConfigChangeQueue* events);
    void stop_dsp();
    bool dsp_running() const { return dsp_.joinable(); }

private:
    Config& cfg_;
    RuntimeControl& control_;
    RuntimeLifecycle& lifecycle_;
    std::atomic<bool>& running_;
    std::unique_ptr<ISampleSource> source_;
    std::thread dsp_;
};

} // namespace famidec
