#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"
#include "streamforge/storage/store.hpp"

namespace streamforge {
namespace report {

enum class Type { Summary, Incident, Quality };
enum class Format { Csv, Json };

// Parses "summary|incident|quality" / "csv|json".
Result<Type> parse_type(const std::string& s);
Result<Format> parse_format(const std::string& s);

struct Request {
    Type type = Type::Summary;
    Format format = Format::Csv;
    std::string device_id; // optional filter
    std::string metric_id; // optional filter
    int64_t from_us = 0;   // required: report window [from, to)
    int64_t to_us = 0;
    std::string output_path; // written via temp file + atomic rename (FR-RPT-002)
};

struct Result_ {
    int64_t rows = 0;
    int64_t generated_at_us = 0;
    int64_t data_cutoff_us = 0; // newest input data at or before `to`
    std::string output_path;
};
using ReportResult = Result_;
// (named Result_ because Result<T> is the project error type)

// Renders the report rows and atomically writes them: content is streamed to a
// sibling temporary file, flushed, closed and only then renamed over the target.
// CSV cells beginning with = + - @ are prefixed with a single quote so spreadsheet
// applications do not interpret them as formulas (FR-RPT-002).
Result<ReportResult> generate(storage::Store& store, const ConfigSnapshot& cs, const Request& req);

} // namespace report
} // namespace streamforge
