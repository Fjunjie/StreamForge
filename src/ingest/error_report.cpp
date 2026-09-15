#include "streamforge/ingest/error_report.hpp"

#include <nlohmann/json.hpp>

#include "streamforge/core/time.hpp"

namespace streamforge {

using nlohmann::json;

std::string build_error_report(const std::string& file_path, const std::string& identity_hash,
                               const std::string& error_code, const std::string& summary, int64_t total_records,
                               int64_t business_errors, int64_t format_errors, double error_rate,
                               uint64_t config_version, const std::vector<SampleError>& sample_errors) {
    json report;
    report["file"] = file_path;
    report["identity_hash"] = identity_hash;
    report["error_code"] = error_code;
    report["summary"] = summary;
    report["total_records"] = total_records;
    report["business_errors"] = business_errors;
    report["format_errors"] = format_errors;
    report["error_rate"] = error_rate;
    report["config_version"] = config_version;
    report["quarantined_at"] = format_utc_us(now_time());
    json errors = json::array();
    for (const auto& e : sample_errors) {
        json item;
        item["line"] = e.line_no;
        item["code"] = e.code;
        item["message"] = e.message;
        item["raw"] = e.raw;
        errors.push_back(std::move(item));
    }
    report["sample_errors"] = std::move(errors);
    return report.dump(2);
}

} // namespace streamforge
