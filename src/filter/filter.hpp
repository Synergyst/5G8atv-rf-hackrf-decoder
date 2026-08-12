#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../dsp/frame.hpp"

namespace famidec {

// Adjust RGB luminance while preserving chroma in BT.601-like YUV space.
// Reconstructing RGB from Y/U/V avoids the channel-wise clipping artifacts
// caused by simply adding a luma delta independently to R, G and B.
inline uint32_t replace_luma_preserve_chroma(uint32_t pixel, uint8_t new_y) {
    const float r = static_cast<float>(pixel & 0xffu);
    const float g = static_cast<float>((pixel >> 8) & 0xffu);
    const float b = static_cast<float>((pixel >> 16) & 0xffu);
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    const float u = (b - y) / 1.772f;
    const float v = (r - y) / 1.402f;
    const float yy = static_cast<float>(new_y);
    const auto q = [](float x) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(x, 0.0f, 255.0f) + 0.5f);
    };
    const uint32_t rr = q(yy + 1.402f * v);
    const uint32_t gg = q(yy - 0.344136f * u - 0.714136f * v);
    const uint32_t bb = q(yy + 1.772f * u);
    return 0xff000000u | (bb << 16) | (gg << 8) | rr;
}

// ─── IFilter ─────────────────────────────────────────────────────────────────
// Base class for all video filters. Every filter operates in-place on a Frame.
// The pipeline calls process() in registration order.

class IFilter {
public:
    virtual ~IFilter() = default;

    // Initialize/reconfigure the filter. Called once after construction or
    // when the config changes.
    virtual void init(int width, int height) = 0;

    // Process a frame in-place.
    virtual void process(Frame& frame) = 0;

    // Enable/disable the filter. A disabled filter passes the frame through.
    bool enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }

    // Human-readable name.
    virtual const char* name() const = 0;

    // Whether this filter needs the *previous* frame as a reference.
    virtual bool needs_reference_frame() const { return false; }

protected:
    bool enabled_ = true;
};

// ─── FilterPipeline ──────────────────────────────────────────────────────────
// Manages a chain of IFilter* objects. Each filter's process() is called in
// sequence, modifying the frame in-place.

class FilterPipeline {
public:
    FilterPipeline() = default;
    ~FilterPipeline() = default;

    // Add a filter to the pipeline. The pipeline takes ownership.
    void add(IFilter* filter);

    // Initialize all filters to the given dimensions.
    void init(int width, int height);

    // Check if the pipeline is empty (no filters).
    bool empty() const { return filters_.empty(); }

    // Process the frame through the entire pipeline.
    void process(Frame& frame);

    // Clear all filters.
    void clear();

    // Rebuild the pipeline in place while preserving its current dimensions.
    // Used by runtime configuration updates from the Web UI.
    void reconfigure(int width, int height) { init(width, height); }

    // Access the reference frame (only valid if any filter needs it).
    const Frame* reference_frame() const { return ref_frame_.get(); }

private:
    // Accessible by FilterPipeline member functions within the same class.
    std::vector<std::unique_ptr<IFilter>> filters_;
    std::unique_ptr<Frame> ref_frame_;
};

// ─── Factory / Registry ──────────────────────────────────────────────────────
// Allows new filters to be "plug-in" style: define the filter class, add
// one line to register it, and the pipeline builder can instantiate it
// by name from Config.

class FilterFactory {
public:
    virtual ~FilterFactory() = default;
    virtual IFilter* create() const = 0;
    virtual const char* name() const = 0;
};

class FilterRegistry {
public:
    // Register a factory. Thread-safe via static local mutex.
    static void register_factory(const FilterFactory* factory);

    // Create a filter by name. Returns nullptr if not found.
    static IFilter* create(const std::string& name);

    // List all registered filter names.
    static std::vector<std::string> list_names();

private:
    // Static storage: map of name -> factory pointer.
    // Thread-safe via the mutex in the struct.
    struct FactoryStore {
        std::unordered_map<std::string, const FilterFactory*> map;
        std::mutex mtx;
    };
    static FactoryStore& store();
};

}  // namespace famidec

// ─── Auto-registration macro ─────────────────────────────────────────────────
// Usage in your filter's .cpp file (at namespace scope, outside any namespace):
//   REGISTER_FILTER(MyFilter);
//
// This creates a factory class, registers it at program startup via a static
// initializer, and runs the constructor before main().

#define REGISTER_FILTER(Class)                                                 \
    namespace {                                                                  \
        class Class##Factory : public famidec::FilterFactory {                   \
        public:                                                                  \
            famidec::IFilter* create() const override { return new famidec::Class(); }    \
            const char* name() const override { return #Class; }                 \
        };                                                                       \
        static bool Class##_registered = ([]() {                                 \
            famidec::FilterRegistry::register_factory(                           \
                new Class##Factory());                                           \
            return true;                                                         \
        })();                                                                    \
    }
