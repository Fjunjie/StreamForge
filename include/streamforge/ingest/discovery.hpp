#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "streamforge/core/error.hpp"
#include "streamforge/ingest/format.hpp"

namespace streamforge {

// One candidate file produced by a directory scan.
struct ScanHit {
    std::string path;       // absolute path of the candidate
    bool via_ready = false; // discovered through a ".ready" marker
    InputFormat format = InputFormat::Unknown;
};

// Watches one input directory by polling (requirement FR-IN-002).
// A file becomes a candidate when:
//   - its name ends with ".ready" (processed immediately; the format comes from the
//     remaining extension), or
//   - its size and mtime were identical in the previous scan and the current scan and the
//     file has been silent for at least `quiet_period`.
// Temporary files, hidden files, symlinks and unsupported extensions are ignored.
class DirectoryScanner {
public:
    DirectoryScanner(std::string dir, std::chrono::milliseconds quiet_period);

    // Performs one scan pass and returns newly eligible candidates (sorted by path).
    Result<std::vector<ScanHit>> scan();

private:
    struct Observed {
        int64_t size = 0;
        int64_t mtime_us = 0;
        bool seen_before = false;
    };

    std::string dir_;
    std::chrono::milliseconds quiet_period_;
    std::map<std::string, Observed> observed_;
    std::set<std::string> reported_; // already handed to the pipeline this process lifetime
};

} // namespace streamforge
