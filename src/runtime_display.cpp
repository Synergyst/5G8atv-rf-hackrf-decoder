#include "runtime_display.hpp"
namespace famidec {
void RuntimeDisplay::render_filtered(SdlDisplay& display, FilterPipeline& pipeline,
                                      const Frame* frame, const OsdStats& stats,
                                      bool imgui_mode, bool) {
    if (pipeline.empty()) {
        if (imgui_mode) display.render_video_only(frame);
        else display.render(frame, stats);
        return;
    }
    if (!frame) return;
    Frame filtered;
    filtered.width = frame->width;
    filtered.height = frame->height;
    filtered.rgba = frame->rgba;
    filtered.seq = frame->seq;
    pipeline.process(filtered);
    if (imgui_mode) display.render_video_only(&filtered);
    else display.render(&filtered, stats);
}
} // namespace famidec
