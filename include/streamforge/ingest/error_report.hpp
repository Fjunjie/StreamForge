#pragma once

#include <string>
#include <vector>

#include "streamforge/core/error.hpp"
#include "streamforge/ingest/raw_record.hpp"

namespace streamforge {

// Sample error entry for quarantine reports (requirement FR-IN-004).
struct SampleError {
    int64_t line_no = 0;
    std::string code; // error code name
    std::string message;
    std::string raw; // sanitized, <= 256 bytes
};

// Serializes the "<name>.error.json" quarantine report.
std::string build_error_report(const std::string& file_path, const std::string& identity_hash,
                               const std::string& error_code, const std::string& summary, int64_t total_records,
                               int64_t business_errors, int64_t format_errors, double error_rate,
                               uint64_t config_version, const std::vector<SampleError>& sample_errors);

} // namespace streamforge
