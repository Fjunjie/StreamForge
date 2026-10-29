#include "streamforge/report/report.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamforge/core/time.hpp"
#include "streamforge/core/uuid.hpp"

namespace streamforge {
namespace report {

Result<Type> parse_type(const std::string& s) {
    if (s == "summary")
        return Result<Type>::Ok(Type::Summary);
    if (s == "incident")
        return Result<Type>::Ok(Type::Incident);
    if (s == "quality")
        return Result<Type>::Ok(Type::Quality);
    return Result<Type>::Err(Error::make(ErrorCode::InvalidArgument,
                                         "report type must be summary|incident|quality").ctx("value", s));
}

Result<Format> parse_format(const std::string& s) {
    if (s == "csv")
        return Result<Format>::Ok(Format::Csv);
    if (s == "json")
        return Result<Format>::Ok(Format::Json);
    return Result<Format>::Err(Error::make(ErrorCode::InvalidArgument, "report format must be csv|json")
                                   .ctx("value", s));
}

namespace {

using json = nlohmann::json;

std::string iso(int64_t us) {
    return format_utc_us(TimePointUs{std::chrono::duration_cast<TimePointUs::duration>(std::chrono::microseconds(us))});
}

std::string csv_cell(const std::string& v) {
    if (!v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '-' || v[0] == '@'))
        return "'" + v; // spreadsheet formula guard (FR-RPT-002)
    return v;
}

std::string csv_num(double v) {
    std::ostringstream out;
    out << v;
    return out.str();
}

// One report row as ordered header/value pairs; the same model renders to CSV and JSON.
using Row = std::vector<std::pair<std::string, std::string>>;

struct Meta {
    std::string type;
    int64_t config_version = 0;
    int64_t from_us = 0;
    int64_t to_us = 0;
    int64_t generated_at_us = 0;
    int64_t data_cutoff_us = 0;
};

std::vector<std::pair<std::string, std::string>> meta_fields(const Meta& m) {
    return {{"report_type", m.type},
            {"config_version", std::to_string(m.config_version)},
            {"from", iso(m.from_us)},
            {"to", iso(m.to_us)},
            {"generated_at", iso(m.generated_at_us)},
            {"data_cutoff", iso(m.data_cutoff_us)}};
}

// ---------------------------------------------------------------- row builders

// Summary: per (device, metric) sample statistics plus missing intervals (FR-RPT-001).
Result<std::vector<Row>> summary_rows(storage::Store& store, const Request& req, const Meta& meta) {
    auto stats = store.summary_by_device_metric(req.device_id, req.metric_id, req.from_us, req.to_us);
    if (!stats.ok())
        return Result<std::vector<Row>>::Err(stats.error());
    auto gaps = store.missing_summary(req.device_id, req.metric_id, req.from_us, req.to_us);
    if (!gaps.ok())
        return Result<std::vector<Row>>::Err(gaps.error());

    std::vector<Row> rows;
    for (const auto& s : stats.value()) {
        Row row = meta_fields(meta);
        row.emplace_back("device_id", s.device_id);
        row.emplace_back("metric_id", s.metric_id);
        row.emplace_back("sample_count", std::to_string(s.sample_count));
        row.emplace_back("null_count", std::to_string(s.null_count));
        row.emplace_back("bad_quality_count", std::to_string(s.bad_quality_count));
        if (s.min_value)
            row.emplace_back("min", csv_num(*s.min_value));
        if (s.max_value)
            row.emplace_back("max", csv_num(*s.max_value));
        if (s.avg_value)
            row.emplace_back("avg", csv_num(*s.avg_value));
        rows.push_back(std::move(row));
    }
    for (const auto& g : gaps.value()) {
        Row row = meta_fields(meta);
        row.emplace_back("device_id", g.device_id);
        row.emplace_back("metric_id", g.metric_id);
        row.emplace_back("missing_intervals", std::to_string(g.intervals));
        row.emplace_back("missing_expected_count", std::to_string(g.expected_count));
        rows.push_back(std::move(row));
    }
    return Result<std::vector<Row>>::Ok(std::move(rows));
}

// Incident report: one row per incident with severity/rule/device roll-up columns.
Result<std::vector<Row>> incident_rows(storage::Store& store, const Request& req, const Meta& meta) {
    storage::IncidentFilter filter;
    filter.device_id = req.device_id;
    filter.from_us = req.from_us;
    filter.to_us = req.to_us;
    filter.limit = 100000;
    auto incidents = store.query_incidents_filtered(filter);
    if (!incidents.ok())
        return Result<std::vector<Row>>::Err(incidents.error());
    std::vector<Row> rows;
    for (const auto& i : incidents.value()) {
        Row row = meta_fields(meta);
        row.emplace_back("incident_id", i.uuid);
        row.emplace_back("rule_id", i.rule_id);
        row.emplace_back("rule_version", std::to_string(i.rule_version));
        row.emplace_back("device_id", i.device_id);
        row.emplace_back("severity", i.severity);
        row.emplace_back("state", i.state);
        row.emplace_back("started_at", iso(i.started_us));
        if (i.ended_us)
            row.emplace_back("ended_at", iso(*i.ended_us));
        row.emplace_back("hit_count", std::to_string(i.hit_count));
        row.emplace_back("reopen_count", std::to_string(i.reopen_count));
        row.emplace_back("acked_by", i.acked_by);
        row.emplace_back("ack_comment", i.ack_comment);
        rows.push_back(std::move(row));
    }
    return Result<std::vector<Row>>::Ok(std::move(rows));
}

// Quality report: ingestion/quality counters over the report window (FR-RPT-001).
Result<std::vector<Row>> quality_rows(storage::Store& store, const Request& req, const Meta& meta) {
    auto counts = store.quality_summary(req.from_us, req.to_us);
    if (!counts.ok())
        return Result<std::vector<Row>>::Err(counts.error());
    std::vector<Row> rows;
    Row row = meta_fields(meta);
    row.emplace_back("records", std::to_string(counts.value().records));
    row.emplace_back("accepted", std::to_string(counts.value().accepted));
    row.emplace_back("duplicates", std::to_string(counts.value().duplicates));
    row.emplace_back("late_samples", std::to_string(counts.value().late_samples));
    row.emplace_back("interpolated_samples", std::to_string(counts.value().interpolated_samples));
    row.emplace_back("derived_samples", std::to_string(counts.value().derived_samples));
    row.emplace_back("missing_intervals", std::to_string(counts.value().missing_intervals));
    row.emplace_back("format_errors", std::to_string(counts.value().format_errors));
    row.emplace_back("business_errors", std::to_string(counts.value().business_errors));
    rows.push_back(std::move(row));
    return Result<std::vector<Row>>::Ok(std::move(rows));
}

Result<int64_t> write_csv(const std::string& path, const std::vector<Row>& rows) {
    if (rows.empty())
        return Result<int64_t>::Ok(0);
    std::string tmp = path + ".tmp-" + uuid_v4().substr(0, 8);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return Result<int64_t>::Err(Error::make(ErrorCode::IoOpen, "report temp file could not be opened")
                                            .ctx("file", tmp));
        bool first = true;
        for (const auto& [name, value] : rows.front()) {
            if (!first)
                out << ',';
            first = false;
            out << name;
        }
        out << "\r\n";
        for (const auto& row : rows) {
            first = true;
            for (const auto& [name, value] : row) {
                if (!first)
                    out << ',';
                first = false;
                out << csv_cell(value);
            }
            out << "\r\n";
        }
        out.flush();
        if (!out.good())
            return Result<int64_t>::Err(Error::make(ErrorCode::IoWrite, "report temp file write failed"));
    }
    // Atomic publish: rename only after the temp file is flushed and closed.
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return Result<int64_t>::Err(Error::make(ErrorCode::IoRename, "report rename failed").ctx("file", path));
    }
    return Result<int64_t>::Ok(static_cast<int64_t>(rows.size()));
}

Result<int64_t> write_json(const std::string& path, const Meta& meta, const std::vector<Row>& rows) {
    json body;
    body["meta"] = json::object();
    for (const auto& [k, v] : meta_fields(meta))
        body["meta"][k] = v;
    json arr = json::array();
    for (const auto& row : rows) {
        json j = json::object();
        for (const auto& [k, v] : row)
            j[k] = v;
        arr.push_back(std::move(j));
    }
    body["rows"] = std::move(arr);
    std::string tmp = path + ".tmp-" + uuid_v4().substr(0, 8);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return Result<int64_t>::Err(Error::make(ErrorCode::IoOpen, "report temp file could not be opened")
                                            .ctx("file", tmp));
        out << body.dump(2) << "\n";
        out.flush();
        if (!out.good())
            return Result<int64_t>::Err(Error::make(ErrorCode::IoWrite, "report temp file write failed"));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return Result<int64_t>::Err(Error::make(ErrorCode::IoRename, "report rename failed").ctx("file", path));
    }
    return Result<int64_t>::Ok(static_cast<int64_t>(rows.size()));
}

} // namespace

Result<ReportResult> generate(storage::Store& store, const ConfigSnapshot& cs, const Request& req) {
    if (req.from_us <= 0 || req.to_us <= 0 || req.from_us >= req.to_us)
        return Result<ReportResult>::Err(
            Error::make(ErrorCode::InvalidArgument, "report requires 0 < from < to").ctx("location", "report"));
    if (req.output_path.empty())
        return Result<ReportResult>::Err(
            Error::make(ErrorCode::InvalidArgument, "report output path is required").ctx("location", "report"));

    Meta meta;
    meta.type = req.type == Type::Summary   ? "summary"
                : req.type == Type::Incident ? "incident"
                                             : "quality";
    meta.config_version = static_cast<int64_t>(cs.version);
    meta.from_us = req.from_us;
    meta.to_us = req.to_us;
    meta.generated_at_us = now_unix_us();

    auto cutoff = store.data_cutoff_us(req.to_us);
    if (!cutoff.ok())
        return Result<ReportResult>::Err(cutoff.error());
    meta.data_cutoff_us = cutoff.value();

    Result<std::vector<Row>> rows = Result<std::vector<Row>>::Err(
        Error::make(ErrorCode::InternalError, "unreachable report type"));
    if (req.type == Type::Summary)
        rows = summary_rows(store, req, meta);
    else if (req.type == Type::Incident)
        rows = incident_rows(store, req, meta);
    else
        rows = quality_rows(store, req, meta);
    if (!rows.ok())
        return Result<ReportResult>::Err(rows.error());

    Result<int64_t> written = req.format == Format::Csv ? write_csv(req.output_path, rows.value())
                                                        : write_json(req.output_path, meta, rows.value());
    if (!written.ok())
        return Result<ReportResult>::Err(written.error());

    ReportResult out;
    out.rows = written.value();
    out.generated_at_us = meta.generated_at_us;
    out.data_cutoff_us = meta.data_cutoff_us;
    out.output_path = req.output_path;
    return Result<ReportResult>::Ok(out);
}

} // namespace report
} // namespace streamforge
