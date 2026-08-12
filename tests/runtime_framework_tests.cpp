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
    ConfigChangeEvent width{};
    width.type = CFG_FRAME_WIDTH;
    width.val.int_val = 1024;
    control.submit(width);
    assert(control.snapshot().frame_width == 1024);

    RuntimeLifecycle lifecycle;
    RuntimeSession session(cfg, control, lifecycle);
    TripleBuffer frames;
    session.prepare_frames(frames);
    assert(frames.back().width == 1024);
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
