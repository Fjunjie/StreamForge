#pragma once

#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace streamforge {

struct LogConfig {
    std::string level = "info";  // trace|debug|info|warn|error|critical
    std::string format = "text"; // text|json
    std::string file;            // empty: stdout only
    int64_t max_size_mb = 50;    // rotation threshold per file
    int max_files = 5;           // rotated files kept
    bool quiet_console = false;  // suppress stdout sink (useful in tests)
};

// Initializes the logging subsystem. Safe to call again to reconfigure (replaces sinks).
void init_logging(const LogConfig& cfg);

// Returns the logger for a component (name appears as "component" in every record).
// Before init_logging() this returns a default console logger at info level.
std::shared_ptr<spdlog::logger> logger(const std::string& component);

// Flushes and releases loggers before process exit.
void shutdown_logging();

} // namespace streamforge
