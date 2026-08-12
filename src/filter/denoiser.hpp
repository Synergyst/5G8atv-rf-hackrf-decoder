#pragma once

#include "filter.hpp"

namespace famidec {

// 3×3 median spatial denoiser operating on the luminance (Y) channel.
//
// NTSC snow is primarily impulse noise — isolated bright/dark pixels.
// The median is provably optimal for this: within any 3×3 window of a
// snow pixel, the median (5th of 9) will pick a non-noise pixel.
//
// Strength is a linear blend:
//   Y_out = s * median(Y) + (1 - s) * Y
// s=0.0 → off (original frame), s=1.0 → fully median-filtered
//
// U/V channels are untouched — no color smearing.

class Denoiser : public IFilter {
public:
    Denoiser() = default;

    void init(int width, int height) override {
        w_ = width;
        h_ = height;
        const size_t num_pixels = static_cast<size_t>(w_) * static_cast<size_t>(h_);
        // Keep both the input luminance and the output luminance separate. The
        // neighborhood for a pixel must never observe an already-filtered pixel.
        input_y_.resize(num_pixels);
        output_y_.resize(num_pixels);
    }

    void process(Frame& frame) override {
        if (!enabled_) return;

        const size_t W = static_cast<size_t>(w_);
        const size_t H = static_cast<size_t>(h_);
        const size_t N = W * H;

        // Pre-compute Y for every pixel (8-bit fixed-point)
        // Y = 0.299*R + 0.587*G + 0.114*B  (ITU-R BT.601, input is ABGR8888)
        const uint32_t* px = frame.rgba.data();

        for (size_t i = 0; i < N; ++i) {
            uint32_t p = px[i];
            int r = static_cast<int>(p)        & 0xff;
            int g = static_cast<int>((p >> 8)) & 0xff;
            int b = static_cast<int>((p >> 16))& 0xff;
            // Fixed-point: Y = (r*77 + g*150 + b*29) / 256
            int y16 = (r * 77 + g * 150 + b * 29 + 128) >> 8;
            input_y_[i] = static_cast<uint16_t>(std::clamp(y16, 0, 255));
        }

        const uint16_t* src = input_y_.data();
        uint16_t* filtered = output_y_.data();
        const int blend = static_cast<int>(strength_ * 255.0f + 0.5f);

        // 3×3 median with insertion sort. Read exclusively from src and write
        // exclusively to filtered so processing order cannot affect a window.
        for (size_t row = 0; row < H; ++row) {
            for (size_t col = 0; col < W; ++col) {
                // Load 3×3 window (clamped to image edges)
                uint16_t sorted[9];
                int idx = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    int r2 = static_cast<int>(row) + dy;
                    if (r2 < 0) r2 = 0;
                    if (r2 >= static_cast<int>(H)) r2 = static_cast<int>(H) - 1;
                    for (int dx = -1; dx <= 1; ++dx) {
                        int c2 = static_cast<int>(col) + dx;
                        if (c2 < 0) c2 = 0;
                        if (c2 >= static_cast<int>(W)) c2 = static_cast<int>(W) - 1;
                        sorted[idx++] = src[static_cast<size_t>(r2) * W + static_cast<size_t>(c2)];
                    }
                }
                // Insertion sort for 9 elements (optimal for small N)
                for (int i = 1; i < 9; ++i) {
                    uint16_t key = sorted[i];
                    int j = i - 1;
                    while (j >= 0 && sorted[j] > key) {
                        sorted[j + 1] = sorted[j];
                        --j;
                    }
                    sorted[j + 1] = key;
                }
                uint16_t median = sorted[4];  // 5th of 9

                // Blend: s * median + (1 - s) * original
                uint16_t original = src[row * W + col];
                int delta = static_cast<int>(median) - static_cast<int>(original);
                const int scaled_delta = delta * blend;
                const int rounded_delta = scaled_delta >= 0
                    ? (scaled_delta + 127) / 255
                    : (scaled_delta - 127) / 255;
                filtered[row * W + col] = static_cast<uint16_t>(
                    static_cast<int>(original) + rounded_delta);
            }
        }

        // Write back Y into the frame pixels, preserving U/V
        uint32_t* dst = const_cast<uint32_t*>(px);
        for (size_t i = 0; i < N; ++i) {
            uint32_t orig = px[i];
            uint16_t new_y = filtered[i];
            uint16_t old_y = static_cast<uint16_t>(
                ((orig & 0xff) * 77 +
                 ((orig >> 8) & 0xff) * 150 +
                 ((orig >> 16) & 0xff) * 29 + 128) >> 8);
            (void)old_y;
            dst[i] = replace_luma_preserve_chroma(orig,
                                                   static_cast<uint8_t>(new_y));
        }
    }

    // Set denoise strength (0.0 = off, 1.0 = full median)
    void set_strength(float s) { strength_ = std::clamp(s, 0.0f, 1.0f); }

    const char* name() const override { return "denoise"; }

private:
    int w_ = 0, h_ = 0;
    float strength_ = 0.6f;  // default: moderate
    std::vector<uint16_t> input_y_;   // unmodified input luminance for windows
    std::vector<uint16_t> output_y_;  // filtered luminance for the frame
};

}  // namespace famidec
