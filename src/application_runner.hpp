#pragma once

#include <atomic>
#include "config.hpp"

namespace famidec {

extern std::atomic<bool> g_running;
int run_application(Config& config, const Config& startup_baseline);

} // namespace famidec
