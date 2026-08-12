#pragma once

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include "runtime_coordinator.hpp"

namespace famidec {

class RuntimeRecorder final : public IRawRecorder {
public:
    bool start(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) return false;
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) return false;
        path_ = path;
        bytes_ = 0;
        started_ = std::chrono::steady_clock::now();
        return true;
    }

    void write(const uint8_t* data, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_) return;
        const size_t written = std::fwrite(data, 1, size, file_);
        bytes_ += written;
    }

    bool stop(std::string* path = nullptr, uint64_t* bytes = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_) return false;
        std::fclose(file_);
        file_ = nullptr;
        if (path) *path = path_;
        if (bytes) *bytes = bytes_;
        return true;
    }

    bool active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return file_ != nullptr;
    }

    float seconds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_) return 0.0f;
        return std::chrono::duration<float>(
            std::chrono::steady_clock::now() - started_).count();
    }

private:
    mutable std::mutex mutex_;
    std::FILE* file_ = nullptr;
    std::string path_;
    uint64_t bytes_ = 0;
    std::chrono::steady_clock::time_point started_;
};

inline std::string next_recording_path() {
    for (int i = 1; i < 1000; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "fpvdec_rec_%03d.cs8", i);
        std::FILE* file = std::fopen(name, "rb");
        if (file) {
            std::fclose(file);
            continue;
        }
        return name;
    }
    return "fpvdec_rec_overflow.cs8";
}

} // namespace famidec
