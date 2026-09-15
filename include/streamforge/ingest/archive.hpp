#pragma once

#include <string>

#include "streamforge/core/error.hpp"

namespace streamforge {

// Moves a successfully processed file into the archive directory laid out as
// YYYY/MM/DD (UTC). Cross-filesystem moves degrade to copy + verify + delete.
// When the destination exists, a short identity hash is appended to the name and existing
// files are never overwritten (requirement FR-IN-004).
Result<std::string> archive_file(const std::string& path, const std::string& archive_root,
                                 const std::string& identity_hash);

// Moves a rejected file into the quarantine directory and writes "<name>.error.json"
// next to it (requirement FR-IN-004). `error_json` must already be serialized.
Result<std::string> quarantine_file(const std::string& path, const std::string& quarantine_root,
                                    const std::string& identity_hash, const std::string& error_json);

} // namespace streamforge
