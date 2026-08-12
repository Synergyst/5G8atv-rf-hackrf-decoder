#include "runtime_auto_resolution.hpp"
namespace famidec {
void AutoResolution::apply_aspect(Config& cfg, const NtscDecoder&) {
    int w = cfg.frame_width;
    const int h = cfg.frame_height;
    switch (cfg.aspect_ratio) {
        case Config::AspectRatio::R16_9: w = static_cast<int>(h * 16.0 / 9.0 + 0.5); break;
        case Config::AspectRatio::R16_10: w = static_cast<int>(h * 16.0 / 10.0 + 0.5); break;
        case Config::AspectRatio::R5_4: w = static_cast<int>(h * 5.0 / 4.0 + 0.5); break;
        case Config::AspectRatio::R4_3: w = static_cast<int>(h * 4.0 / 3.0 + 0.5); break;
        case Config::AspectRatio::Custom: break;
    }
    cfg.frame_width = w;
    cfg.auto_res_applied = true;
}
} // namespace famidec
