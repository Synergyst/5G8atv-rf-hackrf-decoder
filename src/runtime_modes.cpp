#include "runtime_modes.hpp"
#include "filter/denoiser.hpp"
#include "filter/temporal.hpp"
#include "filter/temporal_median.hpp"
namespace famidec {
void RuntimeModeSupport::build_filters(const Config& cfg, FilterPipeline& pipeline) {
    pipeline.clear();
    if (cfg.denoise > 0.0f) { auto* f = new Denoiser(); f->set_strength(cfg.denoise); pipeline.add(f); }
    if (cfg.denoise_temporal > 0.0f) { auto* f = new TemporalFilter(); f->set_alpha(cfg.denoise_temporal); pipeline.add(f); }
    if (cfg.denoise_temporal_median > 0) { auto* f = new TemporalMedian(); f->set_frames(cfg.denoise_temporal_median); f->set_strength(cfg.denoise_temporal_median_strength); pipeline.add(f); }
    pipeline.init(cfg.frame_width, cfg.frame_height);
}
void RuntimeModeSupport::apply_frame_filters(FilterPipeline& pipeline, const Frame& source, Frame& destination) {
    destination.width = source.width; destination.height = source.height; destination.rgba = source.rgba; destination.seq = source.seq; pipeline.process(destination);
}
} // namespace famidec
