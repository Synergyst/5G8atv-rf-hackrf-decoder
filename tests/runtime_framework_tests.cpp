#include <cassert>
#include <thread>
#include "config.hpp"
#include "runtime_control.hpp"
#include "runtime_lifecycle.hpp"

using namespace famidec;

int main() {
    Config cfg;
    RuntimeControl control(cfg);
    control.with_config([](Config& c) { c.frame_width = 800; });
    assert(control.with_config([](const Config& c) { return c.frame_width; }) == 800);

    RuntimeLifecycle lifecycle;
    assert(!lifecycle.restart_requested());
    lifecycle.request_restart();
    assert(lifecycle.restart_requested());
    lifecycle.clear();
    assert(!lifecycle.restart_requested());
    lifecycle.request_quit();
    assert(lifecycle.quit_requested());
    return 0;
}
