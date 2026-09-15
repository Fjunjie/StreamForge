#pragma once

#include <string>

#include "streamforge/core/error.hpp"

namespace streamforge {

// Returns the normalized absolute path (no symlink resolution of the final component,
// but lexical cleanup plus parent resolution where possible).
Result<std::string> canonical_abs(const std::string& path);

// True when `candidate` (already normalized) equals `root` or lives underneath it.
bool path_within(const std::string& root_canonical, const std::string& candidate_canonical);

// Removes control characters (C0 + DEL) from raw input so it can be logged safely
// (requirement FR-OBS-001). Truncates to `max_bytes`.
std::string sanitize_raw(const std::string& raw, size_t max_bytes);

} // namespace streamforge
