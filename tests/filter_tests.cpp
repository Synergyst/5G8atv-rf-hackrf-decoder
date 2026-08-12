#include <cassert>
#include <cstdint>
#include <iostream>

#include "filter/denoiser.hpp"
#include "filter/temporal.hpp"
#include "filter/temporal_median.hpp"

using namespace famidec;

static uint32_t gray(int y) {
    const uint32_t v = static_cast<uint32_t>(y < 0 ? 0 : y > 255 ? 255 : y);
    return 0xff000000u | (v << 16) | (v << 8) | v;
}

static int red(uint32_t p) { return static_cast<int>(p & 0xffu); }

int main() {
    {
        Frame f;
        f.resize(5, 5);
        std::fill(f.rgba.begin(), f.rgba.end(), gray(100));
        f.rgba[2 * 5 + 2] = gray(255);
        Denoiser filter;
        filter.set_strength(1.0f);
        filter.init(f.width, f.height);
        filter.process(f);
        assert(red(f.rgba[2 * 5 + 2]) < 110);
        assert(red(f.rgba[0]) == 100);
    }

    {
        Frame f;
        f.resize(1, 1);
        TemporalFilter filter;
        filter.set_alpha(0.5f);
        filter.init(1, 1);
        f.rgba[0] = gray(0);
        filter.process(f);
        f.rgba[0] = gray(200);
        filter.process(f);
        assert(red(f.rgba[0]) >= 90 && red(f.rgba[0]) <= 110);
    }

    {
        Frame f;
        f.resize(1, 1);
        TemporalMedian filter;
        filter.set_frames(3);
        filter.set_strength(1.0f);
        filter.init(1, 1);
        f.rgba[0] = gray(0); filter.process(f);
        f.rgba[0] = gray(255); filter.process(f);
        f.rgba[0] = gray(0); filter.process(f);
        assert(red(f.rgba[0]) < 10);
    }

    std::cout << "filter tests passed\n";
    return 0;
}
