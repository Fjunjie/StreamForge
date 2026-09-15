#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "streamforge/core/error.hpp"

namespace date {
class time_zone; // forward declaration keeps date/tz.h out of public headers
}

namespace streamforge {

// Canonical time type: UTC, microsecond precision (requirement 6.1).
using TimePointUs = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

int64_t to_unix_us(TimePointUs tp);
TimePointUs from_unix_us(int64_t us);
int64_t now_unix_us();
TimePointUs now_time();

struct ParsedTime {
    TimePointUs tp{};
    bool leap_second = false; // input second field was ":60"
};

// Parses an event time. Accepted forms:
//   - Unix epoch milliseconds (pure integer, optional leading '-').
//   - ISO 8601 with a UTC offset ("2026-08-01T09:15:30.125+08:00", "…Z").
//   - ISO 8601 without offset; interpreted in `device_tz` (required then).
// DST handling: nonexistent local times are rejected; ambiguous local times resolve to the
// earlier or later offset per `ambiguous_earlier`. Leap seconds (":60") normalize to the start
// of the next minute plus any fraction, and set leap_second.
Result<ParsedTime> parse_event_time(const std::string& raw, const date::time_zone* device_tz, bool ambiguous_earlier);

// Formats a timestamp as UTC ISO 8601 with exactly six fractional digits, e.g.
// "2026-08-01T01:15:30.125000Z" (API convention, requirement FR-API-001).
std::string format_utc_us(TimePointUs tp);

// Parses an administrative timestamp (CLI --from/--to): ISO 8601 with offset, or without
// offset interpreted as UTC.
Result<TimePointUs> parse_admin_time(const std::string& raw);

// Resolves an IANA zone name via the OS tz database; returns nullptr when unknown.
// Results are cached (the date library lookup is not free).
const date::time_zone* locate_timezone(const std::string& name);

} // namespace streamforge
