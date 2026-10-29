#pragma once

#include <cstdint>
#include <string>

#include "streamforge/core/types.hpp"

namespace streamforge {
namespace processing {

// A sample after calibration, unit conversion and validity checks, ready for dedup,
// reorder and aggregation. `value` is in the metric's canonical unit; `input_unit`
// records the original unit for provenance.
struct NormalizedSample {
    std::string device_id;
    std::string metric_id;
    int64_t event_time_us = 0;
    int64_t ingest_time_us = 0;
    bool value_is_null = false;
    double value = 0.0;
    std::string input_unit;
    int quality = 0;
    bool has_sequence = false;
    uint64_t sequence = 0;
    uint32_t flags = 0; // sample_flags bitmask (late/forced_flush/synthetic/...)
    std::string tags_json = "{}";
    int64_t source_position = 0;
};

// Deterministic textual representation of a sample value for dedup keys: the raw IEEE 754
// bit pattern in lowercase hex (FR-ORD-001 forbids tolerance-based keys). Null values use
// the fixed text "null".
std::string normalized_value_text(bool value_is_null, double value);

} // namespace processing
} // namespace streamforge
