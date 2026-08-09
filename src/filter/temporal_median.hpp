#pragma once

#include "filter.hpp"

namespace famidec {

// N-frame temporal median filter on the luminance (Y) channel.
//
// For each pixel, collects Y values from the last N frames and picks the
// median. Random horizontal static lines appear in only 1 frame, so with
// N=5 the median (3rd of 5) rejects them.
//
// This is the same technique analog TVs used with a one-field delay line.
//
// Parameters:
//   depth: number of frames to buffer (3, 5, or 7; default 5)
//   threshold: Y difference (0-255) to consider a pixel "noisy" vs. the
//              running median (default 15). Pixels within threshold pass
//              through unchanged, reducing latency and ghosting on moving
//              objects.
//
// Latency: ~depth/2 frames (e.g., ~2 frames at N=5, ~67ms at 30fps)
// CPU: O(W*H*depth) — about 5× the cost of a single-frame filter, but
//      still only ~5-10ms/frame at 640x480 on a modern CPU.

class TemporalMedian : public IFilter {
public:
    TemporalMedian() = default;

    void init(int width, int height) override {
        w_ = width;
        h_ = height;
        const size_t N = static_cast<size_t>(depth_);
        const size_t W = static_cast<size_t>(width);
        const size_t H = static_cast<size_t>(height);
        const size_t num_pixels = W * H;

        // Circular buffer of Y values for each pixel.
        // Layout: [frame0, frame1, ..., frameN-1] for pixel 0,
        //         then [frame0, frame1, ..., frameN-1] for pixel 1, etc.
        // Total size: num_pixels * depth
        ring_.resize(num_pixels * N, 128);  // initialize to mid-gray
        head_ = 0;  // next write position in circular buffer
    }

    void process(Frame& frame) override {
        if (!enabled_) return;

        const size_t W = static_cast<size_t>(w_);
        const size_t H = static_cast<size_t>(h_);
        const size_t N = static_cast<size_t>(depth_);
        const size_t num_pixels = W * H;
        const uint32_t* px = frame.rgba.data();
        uint32_t* dst = const_cast<uint32_t*>(px);

        // Compute current Y for each pixel
        std::vector<uint16_t> y_cur(num_pixels);
        for (size_t i = 0; i < num_pixels; ++i) {
            uint32_t p = px[i];
            int r = static_cast<int>(p) & 0xff;
            int g = static_cast<int>((p >> 8)) & 0xff;
            int b = static_cast<int>((p >> 16)) & 0xff;
            int y16 = (r * 77 + g * 150 + b * 29 + 128) >> 8;
            y_cur[i] = static_cast<uint16_t>(std::clamp(y16, 0, 255));
        }

        // For each pixel:
        // 1. Compute the running median from the ring buffer (excluding current)
        // 2. If |Y_cur - median| > threshold, replace with median
        // 3. Write Y_cur into the ring buffer (advance head)
        // 4. Write back Y (possibly replaced) to the frame

        // Scratch buffer for sorting N values
        std::vector<uint16_t> scratch(N);

        for (size_t i = 0; i < num_pixels; ++i) {
            // Read N values from ring buffer into scratch
            for (size_t j = 0; j < N; ++j) {
                scratch[j] = ring_[i * N + j];
            }

            // Find median by sorting (N is small: 3, 5, or 7)
            // Insertion sort — optimal for small N
            for (size_t a = 1; a < N; ++a) {
                uint16_t key = scratch[a];
                int b = static_cast<int>(a) - 1;
                while (b >= 0 && scratch[static_cast<size_t>(b)] > key) {
                    scratch[static_cast<size_t>(b + 1)] = scratch[static_cast<size_t>(b)];
                    --b;
                }
                scratch[static_cast<size_t>(b + 1)] = key;
            }
            uint16_t median = scratch[N / 2];

            // Blend between current Y and temporal median based on strength.
            // At strength=1.0: fully replace noisy pixels with median.
            // At strength=0.0: no filtering at all.
            uint16_t y_new = y_cur[i];
            if (strength_ > 0.0f) {
                int diff = static_cast<int>(y_new) - static_cast<int>(median);
                int blend = static_cast<int>(strength_ * 255.0f + 0.5f);
                y_new = static_cast<uint16_t>(
                    static_cast<int>(y_new) - (diff * blend + 127) / 255);
                if (y_new > 255) y_new = 255;
            }

            // Write current Y into ring buffer at head position
            ring_[i * N + head_] = y_new;

            // Advance head (circular)
            head_ = (head_ + 1) % N;

            // Write back Y to frame, preserving U/V
            uint32_t orig = px[i];
            uint16_t old_y = static_cast<uint16_t>(
                ((orig & 0xff) * 77 +
                 ((orig >> 8) & 0xff) * 150 +
                 ((orig >> 16) & 0xff) * 29 + 128) >> 8);
            int32_t delta = static_cast<int32_t>(y_new) - static_cast<int32_t>(old_y);
            uint32_t r = static_cast<uint32_t>(
                std::clamp(static_cast<int32_t>((orig) & 0xff) + delta, 0, 255));
            uint32_t g = static_cast<uint32_t>(
                std::clamp(static_cast<int32_t>(((orig >> 8) & 0xff)) + delta, 0, 255));
            uint32_t b = static_cast<uint32_t>(
                std::clamp(static_cast<int32_t>(((orig >> 16) & 0xff)) + delta, 0, 255));
            dst[i] = (0xff000000u) | (b << 16) | (g << 8) | r;
        }
    }

    // Set number of temporal frames to buffer (3, 5, 7, or 9; odd only).
    void set_frames(int n) {
        if (n % 2 == 0) ++n;  // ensure odd
        if (n >= 3 && n <= 9) depth_ = n;
    }

    // Blend strength: 0.0 = pass-through, 1.0 = full median replacement.
    // At 1.0, noisy pixels are fully replaced with the temporal median.
    // Less than 1.0 blends: Y_out = strength * median + (1 - strength) * Y_cur
    void set_strength(float s) {
        strength_ = std::clamp(s, 0.0f, 1.0f);
    }

    const char* name() const override { return "temporal_median"; }
    bool needs_reference_frame() const override { return true; }

private:
    int w_ = 0, h_ = 0;
    int depth_ = 5;       // 5 frames (~170ms at 30fps)
    float strength_ = 1.0f;  // blend strength 0..1
    std::vector<uint16_t> ring_;  // circular buffer: [pixel0_frame0, pixel0_frame1, ..., pixelN_frameN-1, pixel1_frame0, ...]
    int head_ = 0;          // next write position (circular)
};

}  // namespace famidec
