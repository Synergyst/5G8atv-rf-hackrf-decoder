#include <cassert>
#include <thread>
#include "config.hpp"
#include "runtime_control.hpp"
#include "runtime_lifecycle.hpp"
#include "runtime_session.hpp"
using namespace famidec;

int main() {
    Config cfg;
    RuntimeControl control(cfg);
    control.with_config([](Config& c) { c.frame_width = 800; });
    assert(control.with_config([](const Config& c) { return c.frame_width; }) == 800);

    RuntimeLifecycle lifecycle;
    RuntimeSession session(cfg, control, lifecycle);
    TripleBuffer frames;
    session.prepare_frames(frames);
    assert(frames.back().width == 800);
    assert(frames.back().height == 480);
    assert(!lifecycle.restart_requested());
    lifecycle.request_restart();
    assert(lifecycle.restart_requested());
    lifecycle.clear();
    assert(!lifecycle.restart_requested());
    lifecycle.request_quit();
    assert(lifecycle.quit_requested());
    return 0;
}
