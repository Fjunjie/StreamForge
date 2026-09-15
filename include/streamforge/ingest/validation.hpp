#pragma once

#include <string>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/ingest/raw_record.hpp"

namespace streamforge {

// Basic validation outcome (requirement FR-VAL-001):
//   Accepted       - record passes all checks
//   FormatError    - severe structural error: skipped, does not count toward error rate
//   BusinessError  - business rule violation: skipped, counts toward the file error rate
enum class ValidationKind { Accepted, FormatError, BusinessError };

struct ValidationOutcome {
    ValidationKind kind = ValidationKind::Accepted;
    Error error; // set for both error kinds
    TimePointUs event_time{};
    bool leap_second = false;
};

// Checks 1..7 of FR-VAL-001 in order. `ingest_time` bounds the event time window.
ValidationOutcome validate_record(const RawRecord& rec, const ConfigSnapshot& cs, TimePointUs ingest_time);

} // namespace streamforge
