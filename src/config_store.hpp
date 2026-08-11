#pragma once

#include "config.hpp"

namespace famidec {

// Load persisted settings into cfg. Missing or malformed files are ignored so
// first-run startup still uses built-in defaults.
bool load_config_file(Config& cfg, const std::string& path);

// Persist the current user-visible configuration. Returns false on I/O error.
bool save_config_file(const Config& cfg, const std::string& path);

}  // namespace famidec
