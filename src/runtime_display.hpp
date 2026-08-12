#pragma once
#include "config.hpp"
#include "dsp/frame.hpp"
#include "filter/filter.hpp"
#include "ui/sdl_display.hpp"
namespace famidec {
class RuntimeDisplay {
public:
    static void render_filtered(SdlDisplay& display, FilterPipeline& pipeline,
                                const Frame* frame, const OsdStats& stats,
                                bool imgui_mode, bool crt_mode);
};
} // namespace famidec
