#pragma once
#include "config.hpp"
#include "dsp/frame.hpp"
#include "filter/filter.hpp"
namespace famidec {
class RuntimeModeSupport {
public:
    static void build_filters(const Config& cfg, FilterPipeline& pipeline);
    static void apply_frame_filters(FilterPipeline& pipeline, const Frame& source, Frame& destination);
};
} // namespace famidec
