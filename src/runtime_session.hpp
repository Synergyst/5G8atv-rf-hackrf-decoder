#pragma once

#include <memory>
#include <mutex>
#include "config.hpp"
#include "dsp/frame.hpp"
#include "runtime_control.hpp"
#include "runtime_lifecycle.hpp"
#include "source/sample_source.hpp"

namespace famidec {

// Owns the objects that form one live application generation. The caller must
// stop/join the DSP worker before destroying or resizing the session. This
// narrow ownership object is deliberately independent of GUI implementation.
class RuntimeSession {
public:
    RuntimeSession(Config& config, RuntimeControl& control,
                   RuntimeLifecycle& lifecycle)
        : config_(config), control_(control), lifecycle_(lifecycle) {}

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;

    Config snapshot() const { return control_.snapshot(); }
    RuntimeControl& control() { return control_; }
    RuntimeLifecycle& lifecycle() { return lifecycle_; }

    void prepare_frames(TripleBuffer& frames) const {
        const Config cfg = snapshot();
        frames.resize(cfg.frame_width, cfg.frame_height);
    }

private:
    Config& config_;
    RuntimeControl& control_;
    RuntimeLifecycle& lifecycle_;
};

} // namespace famidec
