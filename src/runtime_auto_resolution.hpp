#pragma once
#include "config.hpp"
#include "dsp/ntsc_decoder.hpp"
namespace famidec {
class AutoResolution {
public:
    static void apply_aspect(Config& cfg, const NtscDecoder& decoder);
};
} // namespace famidec
