#pragma once

#include "filter.hpp"

namespace famidec {

// Temporal IIR (infinite impulse response) filter on the luminance channel.
//
// Smoothes frame-to-frame noise by blending the current frame with the
// previous frame:
//   Y_temporal[n] = alpha * Y_current[n] + (1 - alpha) * Y_temporal[n-1]
//
// alpha ranges from 0.0 to 1.0:
//   alpha = 0.0  → infinite memory (very slow, heavy ghosting)
//   alpha = 0.85 → light, responsive (default)
//   alpha = 1.0  → off (no temporal filtering)
//
// Adds ~1 frame of latency but is virtually free on CPU.
// Best paired with the spatial denoiser: median removes impulse noise,
// temporal removes fine grain.

class TemporalFilter : public IFilter {
public:
    TemporalFilter() = default;

    void init(int width, int height) override {
        w_ = width;
        h_ = height;
        // Allocate previous frame buffer
        prev_frame_.resize(static_cast<size_t>(w_) * h_, 0);
    }

    void process(Frame& frame) override {
        if (!enabled_) return;

        const size_t W = static_cast<size_t>(w_);
        const size_t H = static_cast<size_t>(h_);
        const size_t N = W * H;

        // If alpha is 1.0, we're off (no temporal filtering)
        if (alpha_ >= 1.0f) {
            return;
        }

        const uint32_t* px = frame.rgba.data();

        // Compute Y for current frame
        std::vector<uint16_t> y_current(N);
        for (size_t i = 0; i < N; ++i) {
            uint32_t p = px[i];
            int r = static_cast<int>(p) & 0xff;
            int g = static_cast<int>((p >> 8)) & 0xff;
            int b = static_cast<int>((p >> 16)) & 0xff;
            int y16 = (r * 77 + g * 150 + b * 29 + 128) >> 8;
            y_current[i] = static_cast<uint16_t>(std::clamp(y16, 0, 255));
        }

        // Blend with previous frame: Y_new = alpha * Y_cur + (1 - alpha) * Y_prev
        // Fixed-point: Y = (alpha_int * Y_cur + (256 - alpha_int) * Y_prev) / 256
        int alpha_int = static_cast<int>(alpha_ * 255.0f + 0.5f);
        if (alpha_int >= 255) {
            // Effectively off
            return;
        }
        int prev_int = 256 - alpha_int;

        uint32_t* dst = const_cast<uint32_t*>(px);
        for (size_t i = 0; i < N; ++i) {
            uint16_t y_new = static_cast<uint16_t>(
                (alpha_int * static_cast<int>(y_current[i]) +
                 prev_int * static_cast<int>(prev_frame_[i]) + 128) >> 8);
            prev_frame_[i] = y_new;

            // Write back Y into frame pixels, preserving U/V
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

    // Set temporal alpha (0.0 = heavy smoothing, 1.0 = off)
    void set_alpha(float a) { alpha_ = std::clamp(1.0f - a, 0.0f, 1.0f); }

    const char* name() const override { return "temporal"; }
    bool needs_reference_frame() const override { return true; }

private:
    int w_ = 0, h_ = 0;
    float alpha_ = 0.85f;  // default: responsive temporal IIR
    std::vector<uint16_t> prev_frame_;  // previous frame Y values
};

}  // namespace famidec
