#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace streamforge {

// The standard logical fields every input format maps onto (requirement FR-FMT-001).
struct RawRecord {
    std::string device_id;
    std::string metric;
    std::string event_time_raw; // as written: ISO 8601 or Unix milliseconds
    bool has_value = false;
    bool value_is_null = false;
    double value = 0.0;
    std::string unit;
    bool has_quality = false;
    int quality = 0;
    bool has_sequence = false;
    uint64_t sequence = 0;
    std::vector<std::pair<std::string, std::string>> tags;

    // JSON Lines only: unknown top-level fields preserved verbatim.
    std::string ext_json;

    // Provenance for checkpoints and error reports.
    int64_t position = 0;   // byte offset where the record starts
    int64_t line_no = 0;    // 1-based logical record number
    std::string raw_prefix; // sanitized, <=256 bytes, for error reports
};

} // namespace streamforge
