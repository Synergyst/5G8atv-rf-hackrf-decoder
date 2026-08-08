#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace famidec {

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> rgba;  // ABGR8888 byte order R,G,B,A in memory
    uint64_t seq = 0;

    Frame() = default;

    // Resize the frame buffer to the specified dimensions.
    void resize(int w, int h) {
        width = w;
        height = h;
        // 0xff000000u is black (BGRA: B=0, G=0, R=0, A=255)
        rgba.resize(w * h, 0xff000000u);
    }
};

// Triple buffer: DSP thread writes into back(), publishes; render thread
// acquires the freshest published frame. Lock-free via atomic index packing.
class TripleBuffer {
public:
    Frame& back() { return bufs_[back_idx_]; }

    void resize(int w, int h) {
        for (auto& buf : bufs_) {
            buf.resize(w, h);
        }
    }

    void publish(uint64_t seq) {
        bufs_[back_idx_].seq = seq;
        // swap back with "middle" slot; mark fresh bit
        int mid = middle_.exchange(back_idx_ | kFresh, std::memory_order_acq_rel);
        back_idx_ = mid & kIdxMask;
    }

    // Returns nullptr if no new frame since last acquire.
    const Frame* acquire() {
        int mid = middle_.load(std::memory_order_acquire);
        if (!(mid & kFresh)) return nullptr;
        mid = middle_.exchange(front_idx_, std::memory_order_acq_rel);
        if (!(mid & kFresh)) return nullptr;  // raced with publish; rare
        front_idx_ = mid & kIdxMask;
        return &bufs_[front_idx_];
    }

private:
    static constexpr int kFresh = 4;
    static constexpr int kIdxMask = 3;
    Frame bufs_[3];
    int back_idx_ = 0;
    int front_idx_ = 2;
    std::atomic<int> middle_{1};
};

}  // namespace famidec
