#include "filter.hpp"

namespace famidec {

// ─── FilterRegistry implementation ──────────────────────────────────────────

void FilterRegistry::register_factory(const FilterFactory* factory) {
    std::lock_guard<std::mutex> lock(store().mtx);
    store().map[factory->name()] = factory;
}

IFilter* FilterRegistry::create(const std::string& name) {
    std::lock_guard<std::mutex> lock(store().mtx);
    auto it = store().map.find(name);
    if (it != store().map.end())
        return it->second->create();
    return nullptr;
}

std::vector<std::string> FilterRegistry::list_names() {
    std::lock_guard<std::mutex> lock(store().mtx);
    std::vector<std::string> names;
    names.reserve(store().map.size());
    for (auto& [k, v] : store().map) names.push_back(k);
    return names;
}

FilterRegistry::FactoryStore& FilterRegistry::store() {
    static FactoryStore s;
    return s;
}

// ─── FilterPipeline implementation ──────────────────────────────────────────

void FilterPipeline::add(IFilter* filter) {
    filters_.emplace_back(filter);
}

void FilterPipeline::init(int width, int height) {
    for (auto& f : filters_) f->init(width, height);
    // Resize reference frame if we have one
    if (ref_frame_ && (ref_frame_->width != width || ref_frame_->height != height)) {
        ref_frame_->resize(width, height);
    }
}

void FilterPipeline::process(Frame& frame) {
    // Before processing: snapshot the pre-filter frame if any filter needs it.
    bool need_ref = false;
    for (auto& f : filters_) {
        if (f->enabled() && f->needs_reference_frame()) { need_ref = true; break; }
    }
    if (need_ref) {
        if (!ref_frame_ || ref_frame_->width != frame.width ||
            ref_frame_->height != frame.height) {
            ref_frame_ = std::make_unique<Frame>();
        }
        // Deep copy the pixel data
        ref_frame_->rgba = frame.rgba;
        ref_frame_->width = frame.width;
        ref_frame_->height = frame.height;
    }

    for (auto& f : filters_) {
        if (!f->enabled()) continue;
        f->process(frame);
    }
}

void FilterPipeline::clear() {
    filters_.clear();
    ref_frame_ = nullptr;
}

}  // namespace famidec
