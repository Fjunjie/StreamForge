#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "streamforge/config/config.hpp"
#include "streamforge/core/types.hpp"
#include "streamforge/storage/db.hpp"

namespace streamforge {
namespace storage {

// Lifecycle of an input file in source_files.status.
enum class FileStatus {
    Pending,
    Stage1Processing, // parsing + validation into staging
    Validated,        // error rate passed; staging not yet consumed
    Stage2Processing, // staging being converted into samples
    Completed,
    Quarantined,
    SourceChanged,
    Missing,
};

const char* file_status_name(FileStatus status);
bool parse_file_status(const std::string& name, FileStatus& out);
// Terminal states that make a re-appearing identical file skippable.
bool file_status_terminal(FileStatus status);
// States whose work can be resumed after a crash (FR-REC-002).
bool file_status_recoverable(FileStatus status);

struct SourceFileRow {
    std::string id;
    std::string path;
    int64_t size_bytes = 0;
    int64_t mtime_us = 0;
    std::string sha256_64k;
    std::string identity_hash;
    std::string format;
    FileStatus status = FileStatus::Pending;
    int64_t record_count = 0;
    int64_t accepted_count = 0;
    int64_t format_errors = 0;
    int64_t business_errors = 0;
    std::string error_summary;
    int64_t config_version = 0;
    int64_t first_seen_at_us = 0;
    std::optional<int64_t> completed_at_us;
};

struct FileCounts {
    int64_t record_count = 0;
    int64_t accepted_count = 0;
    int64_t format_errors = 0;
    int64_t business_errors = 0;
};

struct CheckpointRow {
    int stage = 1;
    int64_t stage1_offset = 0;
    int64_t stage1_line = 0;
    int64_t stage2_cursor = 0;
    int64_t stage1_tlm_seq = 0; // last accepted TLM frame sequence during stage 1
    int64_t updated_at_us = 0;
};

struct StagedRecord {
    int64_t seq = 0; // assigned by SQLite on insert
    int64_t position = 0;
    int64_t line_no = 0;
    int64_t ingest_time_us = 0;
    StagedStatus status = StagedStatus::Accepted;
    std::string device_id;
    std::string metric;
    bool has_event_time = false;
    int64_t event_time_us = 0;
    bool has_value = false;
    bool value_is_null = false;
    double value = 0.0;
    std::string unit;
    int quality = 0;
    bool has_sequence = false;
    uint64_t sequence = 0;
    std::string tags_json = "{}";
    std::string ext_json;
    int error_code = 0;
    std::string error_message;
};

struct SampleRow {
    std::string sample_uuid;
    std::string device_id;
    std::string metric_id;
    int64_t event_time_us = 0;
    int64_t ingest_time_us = 0;
    bool value_is_null = false;
    double value = 0.0;
    std::string input_unit;
    int quality = 0;
    std::string source_file_id;
    int64_t source_position = 0;
    bool has_sequence = false;
    uint64_t sequence = 0;
    uint32_t flags = 0;
    int64_t config_version = 0;
    std::string tags_json = "{}";
    std::string normalized_value;
};

struct SampleQuery {
    std::string device_id;
    std::string metric_id;
    int64_t from_us = 0;
    int64_t to_us = 0;
    int64_t limit = 1000;
    bool exclude_synthetic = false;
};

// Cursor-paginated sample page backing GET /api/v1/samples: rows ordered by
// (event_time_us, id) with id > after_id; has_more signals the next page.
struct SamplePage {
    std::vector<SampleRow> rows;
    int64_t last_id = 0;
    bool has_more = false;
};

struct IncidentRow {
    std::string uuid;
    std::string rule_id;
    int64_t rule_version = 0;
    std::string device_id;
    std::string severity;
    std::string state;
    int64_t started_us = 0;
    std::optional<int64_t> ended_us;
    int64_t hit_count = 0;
    int64_t reopen_count = 0;
    std::string acked_by;
    std::string ack_comment;
};

struct IncidentFilter {
    std::string state;    // empty = any
    std::string severity; // empty = any
    std::string device_id;
    std::string rule_id;
    int64_t from_us = 0; // 0 = unbounded
    int64_t to_us = 0;
    int64_t limit = 100;
};

struct DeviceRow {
    std::string id;
    std::string timezone;
    std::vector<std::pair<std::string, std::string>> tags;
    bool enabled = true;
    int64_t config_version = 0;
};

struct AggregateRow {
    std::string device_id;
    std::string metric_id;
    std::string window_type;
    int64_t start_us = 0;
    int64_t end_us = 0;
    int64_t version = 0;
    int64_t sample_count = 0;
    int64_t valid_count = 0;
    int64_t missing_count = 0;
    int64_t bad_quality_count = 0;
    int64_t synthetic_count = 0;
    std::optional<double> min_value;
    std::optional<double> max_value;
    std::optional<double> avg_value;
    std::optional<double> sum_value;
    std::optional<double> stddev;
    std::optional<double> first_value;
    std::optional<int64_t> first_time_us;
    std::optional<double> last_value;
    std::optional<int64_t> last_time_us;
    std::optional<double> p50;
    std::optional<double> p95;
    std::optional<double> p99;
    bool superseded = false;
    int64_t created_at_us = 0;
};

struct TimelineEntry {
    int64_t at_us = 0;
    std::string from_state;
    std::string to_state;
    std::string reason;
    std::string actor;
    std::string details_json;
};

// Per (device, metric) sample statistics for the summary report.
struct DeviceMetricStats {
    std::string device_id;
    std::string metric_id;
    int64_t sample_count = 0;
    int64_t null_count = 0;
    int64_t bad_quality_count = 0;
    std::optional<double> min_value;
    std::optional<double> max_value;
    std::optional<double> avg_value;
};

struct MissingSummary {
    std::string device_id;
    std::string metric_id;
    int64_t intervals = 0;
    int64_t expected_count = 0;
};

// Ingestion/quality counters for the quality report.
struct QualityStats {
    int64_t records = 0;
    int64_t accepted = 0;
    int64_t duplicates = 0;
    int64_t late_samples = 0;
    int64_t interpolated_samples = 0;
    int64_t derived_samples = 0;
    int64_t missing_intervals = 0;
    int64_t format_errors = 0;
    int64_t business_errors = 0;
};

// Facade over SQLite: catalog sync, file lifecycle, staging, samples, incidents and audit.
// All access is serialized through one connection (single-writer model, requirement FR-DB-003).
class Store {
public:
    static Result<std::shared_ptr<Store>> open(const std::string& db_path);
    static Result<std::shared_ptr<Store>> open_readonly(const std::string& db_path);

    // Writes the configuration catalog into devices/metrics/units/pairs/calibrations.
    // Removed devices are disabled (never deleted, requirement on hot reload semantics).
    Result<void> sync_catalog(const ConfigSnapshot& cs);

    // --- source files ---
    Result<std::optional<SourceFileRow>> find_file_by_identity(const std::string& identity_hash);
    Result<std::optional<SourceFileRow>> find_file_by_id(const std::string& id);
    Result<std::optional<SourceFileRow>> latest_file_by_path(const std::string& path);
    Result<void> insert_source_file(const SourceFileRow& row);
    Result<void> update_file_counts(const std::string& id, const FileCounts& counts);
    Result<void> update_file_status(const std::string& id, FileStatus status,
                                    std::optional<std::string> error_summary = {});
    Result<std::vector<SourceFileRow>> files_in_status(const std::vector<FileStatus>& statuses);
    Result<std::map<std::string, int64_t>> file_status_counts();

    // --- checkpoints ---
    Result<void> upsert_checkpoint(const std::string& file_id, const CheckpointRow& cp);
    Result<std::optional<CheckpointRow>> get_checkpoint(const std::string& file_id);

    // --- staging ---
    // Inserts one batch of staged records, advances the file checkpoint and updates counters
    // in a single transaction (requirement FR-DB-002).
    Result<void> stage_batch(const std::string& file_id, const std::vector<StagedRecord>& rows, const CheckpointRow& cp,
                             const FileCounts& counts);
    struct StagingStats {
        int64_t total = 0;
        int64_t accepted = 0;
        int64_t format_errors = 0;
        int64_t business_errors = 0;
        int64_t max_seq = 0;
    };
    Result<StagingStats> staging_stats(const std::string& file_id);
    Result<std::vector<StagedRecord>> load_staging(const std::string& file_id, int64_t after_seq, int64_t limit);
    Result<void> clear_staging(const std::string& file_id);

    // Atomically marks a file quarantined and drops its staging data (one transaction).
    Result<void> finalize_quarantine(const std::string& file_id, const std::string& error_summary);
    // Inserts a stage-2 sample batch, advances the stage-2 cursor and marks the file
    // PROCESSING_STAGE2 in one transaction.
    Result<void> commit_stage2_batch(const std::string& file_id, const std::vector<SampleRow>& rows,
                                     int64_t new_cursor);
    // Atomically clears staging and marks the file COMPLETED.
    Result<void> finalize_completed(const std::string& file_id);

    // --- missing intervals (M2) ---
    // INSERT OR IGNORE on (device, metric, start): deterministic re-detection after a
    // crash must not create duplicate interval rows. Returns true when inserted.
    Result<bool> insert_missing_interval(const std::string& device_id, const std::string& metric_id, int64_t start_us,
                                         int64_t end_us, int64_t expected_count);

    // --- watermark / series seeding (M2) ---
    // MAX(event_time_us) over the device's samples; nullopt when the device has none.
    Result<std::optional<int64_t>> max_event_time_for_device(const std::string& device_id);

    // --- samples ---
    Result<size_t> insert_samples(const std::vector<SampleRow>& rows);
    Result<int64_t> count_samples(const std::string& device_id, const std::string& metric_id, int64_t from_us,
                                  int64_t to_us);
    Result<int64_t> count_all_samples();
    Result<std::vector<SampleRow>> query_samples(const SampleQuery& q);
    Result<SamplePage> query_samples_page(const std::string& device_id, const std::string& metric_id, int64_t from_us,
                                          int64_t to_us, int64_t after_id, int64_t limit);

    // --- incidents (query and ack only until M3) ---
    Result<std::vector<IncidentRow>> query_incidents(const std::string& state, const std::string& severity,
                                                     int64_t limit);
    // Filtered incident query backing GET /api/v1/incidents.
    Result<std::vector<IncidentRow>> query_incidents_filtered(const IncidentFilter& filter);
    Result<std::optional<IncidentRow>> get_incident(const std::string& incident_uuid);
    Result<std::vector<IncidentRow>> incident_history(const std::string& incident_uuid);
    // State-change timeline of one incident (FR-RULE-003).
    Result<std::vector<TimelineEntry>> incident_timeline(const std::string& incident_uuid);
    Result<bool> ack_incident(const std::string& incident_uuid, const std::string& by, const std::string& comment,
                              int64_t at_us);
    Result<int64_t> count_incidents(const std::string& state);

    // --- catalog and aggregates (API query backing, M4) ---
    Result<std::vector<DeviceRow>> list_devices();
    // Latest-version window aggregates in [from_us, to_us); empty device/metric/window_type = any.
    Result<std::vector<AggregateRow>> query_aggregates(const std::string& device_id, const std::string& metric_id,
                                                       const std::string& window_type, int64_t from_us, int64_t to_us,
                                                       int64_t limit);

    // --- data retention (FR-DB-004, M4) ---
    // Batched deletes for samples, aggregates, closed incidents (history cascades) and
    // audit rows older than the configured retention. Open incidents, rule states and
    // checkpoints are never touched. Each transaction deletes at most batch_rows rows.
    // Returns the total number of deleted rows.
    Result<int64_t> purge_retention(const RetentionConfig& retention, int64_t batch_rows);

    // --- report aggregations (FR-RPT-001, M4) ---
    Result<std::vector<DeviceMetricStats>> summary_by_device_metric(const std::string& device_id,
                                                                    const std::string& metric_id, int64_t from_us,
                                                                    int64_t to_us);
    Result<std::vector<MissingSummary>> missing_summary(const std::string& device_id, const std::string& metric_id,
                                                        int64_t from_us, int64_t to_us);
    Result<QualityStats> quality_summary(int64_t from_us, int64_t to_us);
    // Newest sample at or before `to_us` (report data cutoff); 0 when none.
    Result<int64_t> data_cutoff_us(int64_t to_us);

    // --- operations / audit ---
    Result<void> audit(const std::string& op, const std::string& actor, const std::string& target,
                       const std::string& result, const std::string& request_id, const std::string& details_json);
    Result<std::string> integrity_check();
    Result<int64_t> foreign_key_violations();
    Result<int> schema_version();

    Db& db() { return db_; }

private:
    Db db_;
    std::mutex mu_;
};

} // namespace storage
} // namespace streamforge
