#include <atomic>
#include <cassert>
#include "config.hpp"
#include "runtime_control.hpp"
#include "runtime_coordinator.hpp"
#include "runtime_lifecycle.hpp"

using namespace famidec;

int main() {
    Config cfg;
    cfg.input = Config::Input::File;
    cfg.file_path = "/definitely/not/a/file";
    RuntimeControl control(cfg);
    RuntimeLifecycle lifecycle;
    std::atomic<bool> running{true};
    RuntimeCoordinator coordinator(cfg, control, lifecycle, running);
    assert(coordinator.create_source(false));
    assert(coordinator.source() != nullptr);
    assert(!coordinator.start_source());
    assert(!coordinator.source()->error().empty());
    return 0;
}
