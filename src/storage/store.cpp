#include "streamforge/storage/store.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "streamforge/core/time.hpp"
#include "streamforge/core/uuid.hpp"
#include "streamforge/storage/migrations.hpp"

namespace streamforge {
namespace storage {

using nlohmann::json;

namespace {

const char* kStatusNames[] = {"PENDING",   "PROCESSING_STAGE1", "VALIDATED",      "PROCESSING_STAGE2",
                              "COMPLETED", "QUARANTINED",       "SOURCE_CHANGED", "MISSING"};

} // namespace

const char* file_status_name(FileStatus status) {
    return kStatusNames[static_cast<int>(status)];
}

bool parse_file_status(const std::string& name, FileStatus& out) {
    for (int i = 0; i < static_cast<int>(FileStatus::Missing) + 1; ++i) {
        if (name == kStatusNames[i]) {
            out = static_cast<FileStatus>(i);
            return true;
        }
    }
    return false;
}

bool file_status_terminal(FileStatus status) {
    return status == FileStatus::Completed || status == FileStatus::Quarantined ||
           status == FileStatus::SourceChanged || status == FileStatus::Missing;
}

bool file_status_recoverable(FileStatus status) {
    return status == FileStatus::Stage1Processing || status == FileStatus::Validated ||
           status == FileStatus::Stage2Processing;
}

Result<std::shared_ptr<Store>> Store::open(const std::string& db_path) {
    auto db = Db::open(db_path, false);
    if (!db.ok())
        return Result<std::shared_ptr<Store>>::Err(db.error());
    auto store = std::shared_ptr<Store>(new Store());
    store->db_ = std::move(db.value());
    auto migrated = migrate(store->db_);
    if (!migrated.ok())
        return Result<std::shared_ptr<Store>>::Err(migrated.error());
    return Result<std::shared_ptr<Store>>::Ok(std::move(store));
}

Result<std::shared_ptr<Store>> Store::open_readonly(const std::string& db_path) {
    auto db = Db::open(db_path, true);
    if (!db.ok())
        return Result<std::shared_ptr<Store>>::Err(db.error());
    auto store = std::shared_ptr<Store>(new Store());
    store->db_ = std::move(db.value());
    return Result<std::shared_ptr<Store>>::Ok(std::move(store));
}

Result<void> Store::sync_catalog(const ConfigSnapshot& cs) {
    std::lock_guard<std::mutex> lock(mu_);
    auto txn = Txn::begin(db_);
    if (!txn.ok())
        return Result<void>::Err(txn.error());

    int64_t now = now_unix_us();
    int64_t version = static_cast<int64_t>(cs.version);

    for (const auto& dev : cs.cfg.devices) {
        json tags;
        for (const auto& t : dev.tags)
            tags[t.first] = t.second;
        static const char* kSql = "INSERT INTO devices(id, display_name, timezone, tags_json, enabled, config_version,"
                                  " created_at_us) VALUES(?, ?, ?, ?, 1, ?, ?)"
                                  " ON CONFLICT(id) DO UPDATE SET display_name=excluded.display_name,"
                                  " timezone=excluded.timezone, tags_json=excluded.tags_json, enabled=1,"
                                  " config_version=excluded.config_version";
        auto st = txn.value().prepare(kSql);
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_text(1, dev.id);
        st.value().bind_text(2, dev.id);
        st.value().bind_text(3, dev.timezone);
        st.value().bind_text(4, tags.dump());
        st.value().bind_int64(5, version);
        st.value().bind_int64(6, now);
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "upsert device failed"));
        }
    }
    // Disable devices no longer present in the configuration.
    {
        std::string in_list;
        for (size_t i = 0; i < cs.cfg.devices.size(); ++i) {
            if (!in_list.empty())
                in_list += ", ";
            in_list += "?";
        }
        std::string sql = in_list.empty() ? "UPDATE devices SET enabled = 0 WHERE 1"
                                          : "UPDATE devices SET enabled = 0 WHERE id NOT IN (" + in_list + ")";
        auto st = txn.value().prepare(sql);
        if (!st.ok())
            return Result<void>::Err(st.error());
        int idx = 1;
        for (const auto& dev : cs.cfg.devices) {
            st.value().bind_text(idx++, dev.id);
        }
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "disable devices failed"));
        }
    }

    for (const auto& metric : cs.cfg.metrics) {
        static const char* kSql = "INSERT INTO metrics(id, canonical_unit, valid_min, valid_max, expected_period_us,"
                                  " jitter_us, max_gap_us, interpolation, config_version, created_at_us)"
                                  " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
                                  " ON CONFLICT(id) DO UPDATE SET canonical_unit=excluded.canonical_unit,"
                                  " valid_min=excluded.valid_min, valid_max=excluded.valid_max,"
                                  " expected_period_us=excluded.expected_period_us, jitter_us=excluded.jitter_us,"
                                  " max_gap_us=excluded.max_gap_us, interpolation=excluded.interpolation,"
                                  " config_version=excluded.config_version";
        auto st = txn.value().prepare(kSql);
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_text(1, metric.id);
        st.value().bind_text(2, metric.canonical_unit);
        if (metric.valid_min)
            st.value().bind_double(3, *metric.valid_min);
        else
            st.value().bind_null(3);
        if (metric.valid_max)
            st.value().bind_double(4, *metric.valid_max);
        else
            st.value().bind_null(4);
        if (metric.expected_period_us)
            st.value().bind_int64(5, *metric.expected_period_us);
        else
            st.value().bind_null(5);
        if (metric.jitter_us)
            st.value().bind_int64(6, *metric.jitter_us);
        else
            st.value().bind_null(6);
        if (metric.max_gap_us)
            st.value().bind_int64(7, *metric.max_gap_us);
        else
            st.value().bind_null(7);
        const char* interp = "none";
        if (metric.interpolation == Interpolation::Previous)
            interp = "previous";
        if (metric.interpolation == Interpolation::Linear)
            interp = "linear";
        st.value().bind_text(8, interp);
        st.value().bind_int64(9, version);
        st.value().bind_int64(10, now);
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "upsert metric failed"));
        }
        for (const auto& unit : metric.input_units) {
            auto us = txn.value().prepare("INSERT OR IGNORE INTO metric_units(metric_id, unit) VALUES(?, ?)");
            if (!us.ok())
                return Result<void>::Err(us.error());
            us.value().bind_text(1, metric.id);
            us.value().bind_text(2, unit);
            auto s2 = us.value().step();
            if (!s2.ok() || s2.value() != Stmt::Step::Done) {
                return Result<void>::Err(Error::make(ErrorCode::DbStep, "upsert unit failed"));
            }
        }
    }

    // device_metric pairs: rebuild from configuration.
    {
        auto del = txn.value().exec("DELETE FROM device_metrics");
        if (!del.ok())
            return Result<void>::Err(del.error());
        static const char* kSql = "INSERT OR IGNORE INTO device_metrics(device_id, metric_id) VALUES(?, ?)";
        for (const auto& dev : cs.cfg.devices) {
            std::vector<std::string> metric_ids;
            if (dev.metrics.empty()) {
                for (const auto& m : cs.cfg.metrics)
                    metric_ids.push_back(m.id);
            } else {
                metric_ids = dev.metrics;
            }
            for (const auto& mid : metric_ids) {
                auto st = txn.value().prepare(kSql);
                if (!st.ok())
                    return Result<void>::Err(st.error());
                st.value().bind_text(1, dev.id);
                st.value().bind_text(2, mid);
                auto step = st.value().step();
                if (!step.ok() || step.value() != Stmt::Step::Done) {
                    return Result<void>::Err(Error::make(ErrorCode::DbStep, "insert device_metric failed"));
                }
            }
        }
    }

    // calibrations: rebuild from configuration.
    {
        auto del = txn.value().exec("DELETE FROM calibrations");
        if (!del.ok())
            return Result<void>::Err(del.error());
        static const char* kSql = "INSERT INTO calibrations(device_id, metric_id, seg_min, seg_max, slope, intercept,"
                                  " reject_unmatched) VALUES(?, ?, ?, ?, ?, ?, ?)";
        for (const auto& dc : cs.cfg.calibrations) {
            for (const auto& seg : dc.calibration.segments) {
                auto st = txn.value().prepare(kSql);
                if (!st.ok())
                    return Result<void>::Err(st.error());
                st.value().bind_text(1, dc.device_id);
                st.value().bind_text(2, dc.metric_id);
                st.value().bind_double(3, seg.min_inclusive);
                st.value().bind_double(4, seg.max_exclusive);
                st.value().bind_double(5, seg.slope);
                st.value().bind_double(6, seg.intercept);
                st.value().bind_int64(7, dc.calibration.reject_unmatched ? 1 : 0);
                auto step = st.value().step();
                if (!step.ok() || step.value() != Stmt::Step::Done) {
                    return Result<void>::Err(Error::make(ErrorCode::DbStep, "insert calibration failed"));
                }
            }
        }
    }

    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

namespace {

void bind_file_row(Stmt& st, const SourceFileRow& row) {
    st.bind_text(1, row.id);
    st.bind_text(2, row.path);
    st.bind_int64(3, row.size_bytes);
    st.bind_int64(4, row.mtime_us);
    st.bind_text(5, row.sha256_64k);
    st.bind_text(6, row.identity_hash);
    st.bind_text(7, row.format);
    st.bind_text(8, file_status_name(row.status));
    st.bind_int64(9, row.config_version);
    st.bind_int64(10, row.first_seen_at_us);
}

SourceFileRow read_file_row(Stmt& st) {
    SourceFileRow row;
    row.id = st.column_text(0);
    row.path = st.column_text(1);
    row.size_bytes = st.column_int64(2);
    row.mtime_us = st.column_int64(3);
    row.sha256_64k = st.column_text(4);
    row.identity_hash = st.column_text(5);
    row.format = st.column_text(6);
    parse_file_status(st.column_text(7), row.status);
    row.record_count = st.column_int64(8);
    row.accepted_count = st.column_int64(9);
    row.format_errors = st.column_int64(10);
    row.business_errors = st.column_int64(11);
    row.error_summary = st.column_is_null(12) ? "" : st.column_text(12);
    row.config_version = st.column_int64(13);
    row.first_seen_at_us = st.column_int64(14);
    if (!st.column_is_null(15))
        row.completed_at_us = st.column_int64(15);
    return row;
}

const char* kFileColumns = "id, path, size_bytes, mtime_us, sha256_64k, identity_hash, format, status, record_count,"
                           " accepted_count, format_errors, business_errors, error_summary, config_version,"
                           " first_seen_at_us, completed_at_us";

} // namespace

Result<std::optional<SourceFileRow>> Store::find_file_by_identity(const std::string& identity_hash) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare(std::string("SELECT ") + kFileColumns + " FROM source_files WHERE identity_hash = ?");
    if (!st.ok())
        return Result<std::optional<SourceFileRow>>::Err(st.error());
    st.value().bind_text(1, identity_hash);
    auto step = st.value().step();
    if (!step.ok())
        return Result<std::optional<SourceFileRow>>::Err(step.error());
    if (step.value() != Stmt::Step::Row)
        return Result<std::optional<SourceFileRow>>::Ok(std::nullopt);
    return Result<std::optional<SourceFileRow>>::Ok(read_file_row(st.value()));
}

Result<std::optional<SourceFileRow>> Store::find_file_by_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare(std::string("SELECT ") + kFileColumns + " FROM source_files WHERE id = ?");
    if (!st.ok())
        return Result<std::optional<SourceFileRow>>::Err(st.error());
    st.value().bind_text(1, id);
    auto step = st.value().step();
    if (!step.ok())
        return Result<std::optional<SourceFileRow>>::Err(step.error());
    if (step.value() != Stmt::Step::Row)
        return Result<std::optional<SourceFileRow>>::Ok(std::nullopt);
    return Result<std::optional<SourceFileRow>>::Ok(read_file_row(st.value()));
}

Result<std::optional<SourceFileRow>> Store::latest_file_by_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare(std::string("SELECT ") + kFileColumns +
                          " FROM source_files WHERE path = ? ORDER BY first_seen_at_us DESC, id"
                          " LIMIT 1");
    if (!st.ok())
        return Result<std::optional<SourceFileRow>>::Err(st.error());
    st.value().bind_text(1, path);
    auto step = st.value().step();
    if (!step.ok())
        return Result<std::optional<SourceFileRow>>::Err(step.error());
    if (step.value() != Stmt::Step::Row)
        return Result<std::optional<SourceFileRow>>::Ok(std::nullopt);
    return Result<std::optional<SourceFileRow>>::Ok(read_file_row(st.value()));
}

Result<void> Store::insert_source_file(const SourceFileRow& row) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("INSERT INTO source_files(id, path, size_bytes, mtime_us, sha256_64k, identity_hash,"
                          " format, status, config_version, first_seen_at_us)"
                          " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!st.ok())
        return Result<void>::Err(st.error());
    bind_file_row(st.value(), row);
    auto step = st.value().step();
    if (!step.ok() || step.value() != Stmt::Step::Done) {
        return Result<void>::Err(Error::make(ErrorCode::DbConstraint, "insert source file failed: " + db_.last_error())
                                     .ctx("path", row.path));
    }
    return Result<void>::Ok();
}

Result<void> Store::update_file_counts(const std::string& id, const FileCounts& counts) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("UPDATE source_files SET record_count=?, accepted_count=?, format_errors=?,"
                          " business_errors=? WHERE id=?");
    if (!st.ok())
        return Result<void>::Err(st.error());
    st.value().bind_int64(1, counts.record_count);
    st.value().bind_int64(2, counts.accepted_count);
    st.value().bind_int64(3, counts.format_errors);
    st.value().bind_int64(4, counts.business_errors);
    st.value().bind_text(5, id);
    auto step = st.value().step();
    if (!step.ok() || step.value() != Stmt::Step::Done) {
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "update file counts failed"));
    }
    return Result<void>::Ok();
}

Result<void> Store::update_file_status(const std::string& id, FileStatus status,
                                       std::optional<std::string> error_summary) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string sql = "UPDATE source_files SET status=?";
    if (status == FileStatus::Completed)
        sql += ", completed_at_us=?";
    if (error_summary)
        sql += ", error_summary=?";
    sql += " WHERE id=?";
    auto st = db_.prepare(sql);
    if (!st.ok())
        return Result<void>::Err(st.error());
    int idx = 1;
    st.value().bind_text(idx++, file_status_name(status));
    if (status == FileStatus::Completed)
        st.value().bind_int64(idx++, now_unix_us());
    if (error_summary)
        st.value().bind_text(idx++, *error_summary);
    st.value().bind_text(idx++, id);
    auto step = st.value().step();
    if (!step.ok() || step.value() != Stmt::Step::Done) {
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "update file status failed"));
    }
    return Result<void>::Ok();
}

Result<std::vector<SourceFileRow>> Store::files_in_status(const std::vector<FileStatus>& statuses) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string placeholders;
    for (size_t i = 0; i < statuses.size(); ++i) {
        if (!placeholders.empty())
            placeholders += ", ";
        placeholders += "?";
    }
    auto st = db_.prepare(std::string("SELECT ") + kFileColumns + " FROM source_files WHERE status IN (" +
                          placeholders + ") ORDER BY first_seen_at_us");
    if (!st.ok())
        return Result<std::vector<SourceFileRow>>::Err(st.error());
    int idx = 1;
    for (auto s : statuses)
        st.value().bind_text(idx++, file_status_name(s));
    std::vector<SourceFileRow> rows;
    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return Result<std::vector<SourceFileRow>>::Err(step.error());
        if (step.value() != Stmt::Step::Row)
            break;
        rows.push_back(read_file_row(st.value()));
    }
    return Result<std::vector<SourceFileRow>>::Ok(std::move(rows));
}

Result<std::map<std::string, int64_t>> Store::file_status_counts() {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("SELECT status, COUNT(*) FROM source_files GROUP BY status");
    if (!st.ok())
        return Result<std::map<std::string, int64_t>>::Err(st.error());
    std::map<std::string, int64_t> out;
    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return Result<std::map<std::string, int64_t>>::Err(step.error());
        if (step.value() != Stmt::Step::Row)
            break;
        out[st.value().column_text(0)] = st.value().column_int64(1);
    }
    return Result<std::map<std::string, int64_t>>::Ok(std::move(out));
}

Result<void> Store::upsert_checkpoint(const std::string& file_id, const CheckpointRow& cp) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("INSERT INTO checkpoints(file_id, stage, stage1_offset, stage1_line, stage2_cursor,"
                          " updated_at_us) VALUES(?, ?, ?, ?, ?, ?)"
                          " ON CONFLICT(file_id) DO UPDATE SET stage=excluded.stage,"
                          " stage1_offset=excluded.stage1_offset, stage1_line=excluded.stage1_line,"
                          " stage2_cursor=excluded.stage2_cursor, updated_at_us=excluded.updated_at_us");
    if (!st.ok())
        return Result<void>::Err(st.error());
    st.value().bind_text(1, file_id);
    st.value().bind_int64(2, cp.stage);
    st.value().bind_int64(3, cp.stage1_offset);
    st.value().bind_int64(4, cp.stage1_line);
    st.value().bind_int64(5, cp.stage2_cursor);
    st.value().bind_int64(6, cp.updated_at_us);
    auto step = st.value().step();
    if (!step.ok() || step.value() != Stmt::Step::Done) {
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "upsert checkpoint failed"));
    }
    return Result<void>::Ok();
}

Result<std::optional<CheckpointRow>> Store::get_checkpoint(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("SELECT stage, stage1_offset, stage1_line, stage2_cursor, updated_at_us"
                          " FROM checkpoints WHERE file_id = ?");
    if (!st.ok())
        return Result<std::optional<CheckpointRow>>::Err(st.error());
    st.value().bind_text(1, file_id);
    auto step = st.value().step();
    if (!step.ok())
        return Result<std::optional<CheckpointRow>>::Err(step.error());
    if (step.value() != Stmt::Step::Row) {
        return Result<std::optional<CheckpointRow>>::Ok(std::nullopt);
    }
    CheckpointRow cp;
    cp.stage = static_cast<int>(st.value().column_int64(0));
    cp.stage1_offset = st.value().column_int64(1);
    cp.stage1_line = st.value().column_int64(2);
    cp.stage2_cursor = st.value().column_int64(3);
    cp.updated_at_us = st.value().column_int64(4);
    return Result<std::optional<CheckpointRow>>::Ok(cp);
}

Result<void> Store::stage_batch(const std::string& file_id, const std::vector<StagedRecord>& rows,
                                const CheckpointRow& cp, const FileCounts& counts) {
    std::lock_guard<std::mutex> lock(mu_);
    auto txn = Txn::begin(db_);
    if (!txn.ok())
        return Result<void>::Err(txn.error());
    static const char* kSql = "INSERT INTO staging_samples(file_id, position, line_no, ingest_time_us, status,"
                              " device_id, metric, event_time_us, value, value_is_null, has_value, unit, quality,"
                              " sequence, tags_json, ext_json, error_code, error_message)"
                              " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    for (const auto& r : rows) {
        auto st = txn.value().prepare(kSql);
        if (!st.ok())
            return Result<void>::Err(st.error());
        int idx = 1;
        st.value().bind_text(idx++, file_id);
        st.value().bind_int64(idx++, r.position);
        st.value().bind_int64(idx++, r.line_no);
        st.value().bind_int64(idx++, r.ingest_time_us);
        st.value().bind_int64(idx++, static_cast<int64_t>(r.status));
        if (r.device_id.empty())
            st.value().bind_null(idx++);
        else
            st.value().bind_text(idx++, r.device_id);
        if (r.metric.empty())
            st.value().bind_null(idx++);
        else
            st.value().bind_text(idx++, r.metric);
        if (!r.has_event_time)
            st.value().bind_null(idx++);
        else
            st.value().bind_int64(idx++, r.event_time_us);
        if (r.value_is_null || !r.has_value)
            st.value().bind_null(idx++);
        else
            st.value().bind_double(idx++, r.value);
        st.value().bind_int64(idx++, r.value_is_null ? 1 : 0);
        st.value().bind_int64(idx++, r.has_value ? 1 : 0);
        if (r.unit.empty())
            st.value().bind_null(idx++);
        else
            st.value().bind_text(idx++, r.unit);
        st.value().bind_int64(idx++, r.quality);
        if (r.has_sequence)
            st.value().bind_int64(idx++, static_cast<int64_t>(r.sequence));
        else
            st.value().bind_null(idx++);
        st.value().bind_text(idx++, r.tags_json);
        if (r.ext_json.empty())
            st.value().bind_null(idx++);
        else
            st.value().bind_text(idx++, r.ext_json);
        st.value().bind_int64(idx++, r.error_code);
        st.value().bind_text(idx++, r.error_message);
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "stage insert failed: " + db_.last_error()));
        }
    }
    static const char* kCpSql = "INSERT INTO checkpoints(file_id, stage, stage1_offset, stage1_line, stage2_cursor,"
                                " updated_at_us) VALUES(?, ?, ?, ?, ?, ?)"
                                " ON CONFLICT(file_id) DO UPDATE SET stage=excluded.stage,"
                                " stage1_offset=excluded.stage1_offset, stage1_line=excluded.stage1_line,"
                                " stage2_cursor=excluded.stage2_cursor, updated_at_us=excluded.updated_at_us";
    auto cp_st = txn.value().prepare(kCpSql);
    if (!cp_st.ok())
        return Result<void>::Err(cp_st.error());
    cp_st.value().bind_text(1, file_id);
    cp_st.value().bind_int64(2, cp.stage);
    cp_st.value().bind_int64(3, cp.stage1_offset);
    cp_st.value().bind_int64(4, cp.stage1_line);
    cp_st.value().bind_int64(5, cp.stage2_cursor);
    cp_st.value().bind_int64(6, now_unix_us());
    auto cp_step = cp_st.value().step();
    if (!cp_step.ok() || cp_step.value() != Stmt::Step::Done) {
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "checkpoint upsert failed"));
    }
    auto cnt_st = txn.value().prepare("UPDATE source_files SET record_count=?, accepted_count=?, format_errors=?,"
                                      " business_errors=? WHERE id=?");
    if (!cnt_st.ok())
        return Result<void>::Err(cnt_st.error());
    cnt_st.value().bind_int64(1, counts.record_count);
    cnt_st.value().bind_int64(2, counts.accepted_count);
    cnt_st.value().bind_int64(3, counts.format_errors);
    cnt_st.value().bind_int64(4, counts.business_errors);
    cnt_st.value().bind_text(5, file_id);
    auto cnt_step = cnt_st.value().step();
    if (!cnt_step.ok() || cnt_step.value() != Stmt::Step::Done) {
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "file counts update failed"));
    }
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

Result<Store::StagingStats> Store::staging_stats(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("SELECT status, COUNT(*), MAX(seq) FROM staging_samples WHERE file_id=? GROUP BY status");
    if (!st.ok())
        return Result<StagingStats>::Err(st.error());
    st.value().bind_text(1, file_id);
    StagingStats stats;
    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return Result<StagingStats>::Err(step.error());
        if (step.value() != Stmt::Step::Row)
            break;
        int64_t status = st.value().column_int64(0);
        int64_t count = st.value().column_int64(1);
        int64_t max_seq = st.value().column_int64(2);
        stats.total += count;
        stats.max_seq = std::max(stats.max_seq, max_seq);
        if (status == static_cast<int64_t>(StagedStatus::Accepted))
            stats.accepted = count;
        if (status == static_cast<int64_t>(StagedStatus::FormatError))
            stats.format_errors = count;
        if (status == static_cast<int64_t>(StagedStatus::BusinessError))
            stats.business_errors = count;
    }
    return Result<StagingStats>::Ok(stats);
}

Result<std::vector<StagedRecord>> Store::load_staging(const std::string& file_id, int64_t after_seq, int64_t limit) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("SELECT seq, position, line_no, ingest_time_us, status, device_id, metric, event_time_us,"
                          " value, value_is_null, has_value, unit, quality, sequence, tags_json, ext_json,"
                          " error_code, error_message FROM staging_samples WHERE file_id=? AND seq>?"
                          " ORDER BY seq LIMIT ?");
    if (!st.ok())
        return Result<std::vector<StagedRecord>>::Err(st.error());
    st.value().bind_text(1, file_id);
    st.value().bind_int64(2, after_seq);
    st.value().bind_int64(3, limit);
    std::vector<StagedRecord> rows;
    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return Result<std::vector<StagedRecord>>::Err(step.error());
        if (step.value() != Stmt::Step::Row)
            break;
        StagedRecord r;
        r.seq = st.value().column_int64(0);
        r.position = st.value().column_int64(1);
        r.line_no = st.value().column_int64(2);
        r.ingest_time_us = st.value().column_int64(3);
        r.status = static_cast<StagedStatus>(st.value().column_int64(4));
        if (!st.value().column_is_null(5))
            r.device_id = st.value().column_text(5);
        if (!st.value().column_is_null(6))
            r.metric = st.value().column_text(6);
        r.has_event_time = !st.value().column_is_null(7);
        if (r.has_event_time)
            r.event_time_us = st.value().column_int64(7);
        r.value = st.value().column_double(8);
        r.value_is_null = st.value().column_int64(9) != 0;
        r.has_value = st.value().column_int64(10) != 0;
        if (!st.value().column_is_null(11))
            r.unit = st.value().column_text(11);
        r.quality = static_cast<int>(st.value().column_int64(12));
        if (!st.value().column_is_null(13)) {
            r.has_sequence = true;
            r.sequence = static_cast<uint64_t>(st.value().column_int64(13));
        }
        r.tags_json = st.value().column_text(14);
        if (!st.value().column_is_null(15))
            r.ext_json = st.value().column_text(15);
        r.error_code = static_cast<int>(st.value().column_int64(16));
        r.error_message = st.value().column_text(17);
        rows.push_back(std::move(r));
    }
    return Result<std::vector<StagedRecord>>::Ok(std::move(rows));
}

Result<void> Store::clear_staging(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("DELETE FROM staging_samples WHERE file_id=?");
    if (!st.ok())
        return Result<void>::Err(st.error());
    st.value().bind_text(1, file_id);
    auto step = st.value().step();
    if (!step.ok() || step.value() != Stmt::Step::Done) {
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "clear staging failed"));
    }
    return Result<void>::Ok();
}

Result<void> Store::finalize_quarantine(const std::string& file_id, const std::string& error_summary) {
    std::lock_guard<std::mutex> lock(mu_);
    auto txn = Txn::begin(db_);
    if (!txn.ok())
        return Result<void>::Err(txn.error());
    {
        auto st = txn.value().prepare("DELETE FROM staging_samples WHERE file_id=?");
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_text(1, file_id);
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "clear staging failed"));
        }
    }
    {
        auto st = txn.value().prepare("UPDATE source_files SET status='QUARANTINED', error_summary=? WHERE id=?");
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_text(1, error_summary);
        st.value().bind_text(2, file_id);
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "quarantine update failed"));
        }
    }
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

namespace {

// INSERT OR IGNORE: the samples-table unique indexes ((device, metric, sequence) and
// (device, metric, event_time, normalized_value)) are the final deduplication defense
// (agreed design); conflicting rows are skipped instead of failing the batch. Returns the
// number of rows actually inserted.
Result<int64_t> insert_sample_rows(Db& db, Txn& txn, const std::vector<SampleRow>& rows) {
    static const char* kSql = "INSERT OR IGNORE INTO samples(sample_uuid, device_id, metric_id, event_time_us,"
                              " ingest_time_us, value, input_unit, quality, source_file_id, source_position, sequence,"
                              " flags, config_version, expression_version, tags_json, normalized_value)"
                              " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, ?, ?)";
    int64_t inserted = 0;
    for (const auto& r : rows) {
        auto st = txn.prepare(kSql);
        if (!st.ok())
            return Result<int64_t>::Err(st.error());
        int idx = 1;
        st.value().bind_text(idx++, r.sample_uuid);
        st.value().bind_text(idx++, r.device_id);
        st.value().bind_text(idx++, r.metric_id);
        st.value().bind_int64(idx++, r.event_time_us);
        st.value().bind_int64(idx++, r.ingest_time_us);
        if (r.value_is_null)
            st.value().bind_null(idx++);
        else
            st.value().bind_double(idx++, r.value);
        if (r.input_unit.empty())
            st.value().bind_null(idx++);
        else
            st.value().bind_text(idx++, r.input_unit);
        st.value().bind_int64(idx++, r.quality);
        if (r.source_file_id.empty())
            st.value().bind_null(idx++);
        else
            st.value().bind_text(idx++, r.source_file_id);
        st.value().bind_int64(idx++, r.source_position);
        if (r.has_sequence)
            st.value().bind_int64(idx++, static_cast<int64_t>(r.sequence));
        else
            st.value().bind_null(idx++);
        st.value().bind_int64(idx++, r.flags);
        st.value().bind_int64(idx++, r.config_version);
        st.value().bind_text(idx++, r.tags_json);
        if (r.normalized_value.empty())
            st.value().bind_null(idx++);
        else
            st.value().bind_text(idx++, r.normalized_value);
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<int64_t>::Err(
                Error::make(ErrorCode::DbConstraint, "sample insert failed: " + db.last_error()));
        }
        inserted += db.changes();
    }
    return Result<int64_t>::Ok(inserted);
}

} // namespace

Result<void> Store::commit_stage2_batch(const std::string& file_id, const std::vector<SampleRow>& rows,
                                        int64_t new_cursor) {
    std::lock_guard<std::mutex> lock(mu_);
    auto txn = Txn::begin(db_);
    if (!txn.ok())
        return Result<void>::Err(txn.error());
    auto inserted = insert_sample_rows(db_, txn.value(), rows);
    if (!inserted.ok())
        return Result<void>::Err(inserted.error());
    {
        auto st =
            txn.value().prepare("INSERT INTO checkpoints(file_id, stage, stage1_offset, stage1_line, stage2_cursor,"
                                " updated_at_us) VALUES(?, 2, 0, 0, ?, ?)"
                                " ON CONFLICT(file_id) DO UPDATE SET stage=2, stage2_cursor=excluded.stage2_cursor,"
                                " updated_at_us=excluded.updated_at_us");
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_text(1, file_id);
        st.value().bind_int64(2, new_cursor);
        st.value().bind_int64(3, now_unix_us());
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "checkpoint upsert failed"));
        }
    }
    {
        auto st = txn.value().prepare("UPDATE source_files SET status='PROCESSING_STAGE2' WHERE id=?");
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_text(1, file_id);
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "status update failed"));
        }
    }
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

Result<void> Store::finalize_completed(const std::string& file_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto txn = Txn::begin(db_);
    if (!txn.ok())
        return Result<void>::Err(txn.error());
    {
        auto st = txn.value().prepare("DELETE FROM staging_samples WHERE file_id=?");
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_text(1, file_id);
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "clear staging failed"));
        }
    }
    {
        auto st = txn.value().prepare("UPDATE source_files SET status='COMPLETED', completed_at_us=? WHERE id=?");
        if (!st.ok())
            return Result<void>::Err(st.error());
        st.value().bind_int64(1, now_unix_us());
        st.value().bind_text(2, file_id);
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<void>::Err(Error::make(ErrorCode::DbStep, "complete update failed"));
        }
    }
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<void>::Err(commit.error());
    return Result<void>::Ok();
}

Result<size_t> Store::insert_samples(const std::vector<SampleRow>& rows) {
    std::lock_guard<std::mutex> lock(mu_);
    auto txn = Txn::begin(db_);
    if (!txn.ok())
        return Result<size_t>::Err(txn.error());
    auto inserted = insert_sample_rows(db_, txn.value(), rows);
    if (!inserted.ok())
        return Result<size_t>::Err(inserted.error());
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<size_t>::Err(commit.error());
    return Result<size_t>::Ok(rows.size());
}

Result<int64_t> Store::count_samples(const std::string& device_id, const std::string& metric_id, int64_t from_us,
                                     int64_t to_us) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("SELECT COUNT(*) FROM samples WHERE device_id=? AND metric_id=? AND event_time_us>=?"
                          " AND event_time_us<?");
    if (!st.ok())
        return Result<int64_t>::Err(st.error());
    st.value().bind_text(1, device_id);
    st.value().bind_text(2, metric_id);
    st.value().bind_int64(3, from_us);
    st.value().bind_int64(4, to_us);
    auto step = st.value().step();
    if (!step.ok() || step.value() != Stmt::Step::Row) {
        return Result<int64_t>::Err(Error::make(ErrorCode::DbStep, "count samples failed"));
    }
    return Result<int64_t>::Ok(st.value().column_int64(0));
}

Result<int64_t> Store::count_all_samples() {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("SELECT COUNT(*) FROM samples");
    if (!st.ok())
        return Result<int64_t>::Err(st.error());
    auto step = st.value().step();
    if (!step.ok() || step.value() != Stmt::Step::Row) {
        return Result<int64_t>::Err(Error::make(ErrorCode::DbStep, "count samples failed"));
    }
    return Result<int64_t>::Ok(st.value().column_int64(0));
}

Result<std::vector<SampleRow>> Store::query_samples(const SampleQuery& q) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string sql = "SELECT sample_uuid, device_id, metric_id, event_time_us, ingest_time_us, value,"
                      " input_unit, quality, source_file_id, source_position, sequence, flags, config_version,"
                      " tags_json, normalized_value FROM samples WHERE device_id=? AND metric_id=?"
                      " AND event_time_us>=? AND event_time_us<?";
    if (q.exclude_synthetic)
        sql += " AND (flags & 4) = 0";
    sql += " ORDER BY event_time_us LIMIT ?";
    auto st = db_.prepare(sql);
    if (!st.ok())
        return Result<std::vector<SampleRow>>::Err(st.error());
    st.value().bind_text(1, q.device_id);
    st.value().bind_text(2, q.metric_id);
    st.value().bind_int64(3, q.from_us);
    st.value().bind_int64(4, q.to_us);
    st.value().bind_int64(5, q.limit);
    std::vector<SampleRow> rows;
    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return Result<std::vector<SampleRow>>::Err(step.error());
        if (step.value() != Stmt::Step::Row)
            break;
        SampleRow r;
        r.sample_uuid = st.value().column_text(0);
        r.device_id = st.value().column_text(1);
        r.metric_id = st.value().column_text(2);
        r.event_time_us = st.value().column_int64(3);
        r.ingest_time_us = st.value().column_int64(4);
        r.value_is_null = st.value().column_is_null(5);
        r.value = st.value().column_double(5);
        if (!st.value().column_is_null(6))
            r.input_unit = st.value().column_text(6);
        r.quality = static_cast<int>(st.value().column_int64(7));
        if (!st.value().column_is_null(8))
            r.source_file_id = st.value().column_text(8);
        r.source_position = st.value().column_int64(9);
        if (!st.value().column_is_null(10)) {
            r.has_sequence = true;
            r.sequence = static_cast<uint64_t>(st.value().column_int64(10));
        }
        r.flags = static_cast<uint32_t>(st.value().column_int64(11));
        r.config_version = st.value().column_int64(12);
        r.tags_json = st.value().column_text(13);
        if (!st.value().column_is_null(14))
            r.normalized_value = st.value().column_text(14);
        rows.push_back(std::move(r));
    }
    return Result<std::vector<SampleRow>>::Ok(std::move(rows));
}

Result<std::vector<IncidentRow>> Store::query_incidents(const std::string& state, const std::string& severity,
                                                        int64_t limit) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string sql = "SELECT incident_uuid, rule_id, rule_version, device_id, severity, state, started_us,"
                      " ended_us, hit_count, reopen_count, COALESCE(acked_by,''), COALESCE(ack_comment,'')"
                      " FROM incidents WHERE 1=1";
    if (!state.empty())
        sql += " AND state=?";
    if (!severity.empty())
        sql += " AND severity=?";
    sql += " ORDER BY started_us DESC LIMIT ?";
    auto st = db_.prepare(sql);
    if (!st.ok())
        return Result<std::vector<IncidentRow>>::Err(st.error());
    int idx = 1;
    if (!state.empty())
        st.value().bind_text(idx++, state);
    if (!severity.empty())
        st.value().bind_text(idx++, severity);
    st.value().bind_int64(idx++, limit);
    std::vector<IncidentRow> rows;
    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return Result<std::vector<IncidentRow>>::Err(step.error());
        if (step.value() != Stmt::Step::Row)
            break;
        IncidentRow r;
        r.uuid = st.value().column_text(0);
        r.rule_id = st.value().column_text(1);
        r.rule_version = st.value().column_int64(2);
        r.device_id = st.value().column_text(3);
        r.severity = st.value().column_text(4);
        r.state = st.value().column_text(5);
        r.started_us = st.value().column_int64(6);
        if (!st.value().column_is_null(7))
            r.ended_us = st.value().column_int64(7);
        r.hit_count = st.value().column_int64(8);
        r.reopen_count = st.value().column_int64(9);
        r.acked_by = st.value().column_text(10);
        r.ack_comment = st.value().column_text(11);
        rows.push_back(std::move(r));
    }
    return Result<std::vector<IncidentRow>>::Ok(std::move(rows));
}

Result<std::vector<IncidentRow>> Store::incident_history(const std::string& incident_uuid) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("SELECT h.incident_uuid, i.rule_id, i.rule_version, i.device_id, i.severity, h.reason,"
                          " h.at_us, NULL, 0, 0, COALESCE(h.actor,''), COALESCE(h.details_json,'')"
                          " FROM incident_history h JOIN incidents i ON i.incident_uuid = h.incident_uuid"
                          " WHERE h.incident_uuid=? ORDER BY h.at_us");
    if (!st.ok())
        return Result<std::vector<IncidentRow>>::Err(st.error());
    st.value().bind_text(1, incident_uuid);
    std::vector<IncidentRow> rows;
    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return Result<std::vector<IncidentRow>>::Err(step.error());
        if (step.value() != Stmt::Step::Row)
            break;
        IncidentRow r;
        r.uuid = st.value().column_text(0);
        r.rule_id = st.value().column_text(1);
        r.rule_version = st.value().column_int64(2);
        r.device_id = st.value().column_text(3);
        r.severity = st.value().column_text(4);
        r.state = st.value().column_text(5);
        r.started_us = st.value().column_int64(6);
        r.acked_by = st.value().column_text(10);
        r.ack_comment = st.value().column_text(11);
        rows.push_back(std::move(r));
    }
    return Result<std::vector<IncidentRow>>::Ok(std::move(rows));
}

Result<bool> Store::ack_incident(const std::string& incident_uuid, const std::string& by, const std::string& comment,
                                 int64_t at_us) {
    std::lock_guard<std::mutex> lock(mu_);
    auto txn = Txn::begin(db_);
    if (!txn.ok())
        return Result<bool>::Err(txn.error());
    {
        auto st =
            txn.value().prepare("UPDATE incidents SET acked_by=?, acked_at_us=?, ack_comment=? WHERE incident_uuid=?");
        if (!st.ok())
            return Result<bool>::Err(st.error());
        st.value().bind_text(1, by);
        st.value().bind_int64(2, at_us);
        st.value().bind_text(3, comment);
        st.value().bind_text(4, incident_uuid);
        auto step = st.value().step();
        if (!step.ok())
            return Result<bool>::Err(step.error());
        if (db_.changes() == 0)
            return Result<bool>::Ok(false);
    }
    {
        auto st = txn.value().prepare("INSERT INTO incident_history(incident_uuid, at_us, from_state, to_state, reason,"
                                      " actor, details_json) VALUES(?, ?, NULL, NULL, 'ACKNOWLEDGED', ?, ?)");
        if (!st.ok())
            return Result<bool>::Err(st.error());
        st.value().bind_text(1, incident_uuid);
        st.value().bind_int64(2, at_us);
        st.value().bind_text(3, by);
        json details;
        details["comment"] = comment;
        st.value().bind_text(4, details.dump());
        auto step = st.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<bool>::Err(Error::make(ErrorCode::DbStep, "insert incident history failed"));
        }
    }
    auto commit = txn.value().commit();
    if (!commit.ok())
        return Result<bool>::Err(commit.error());
    return Result<bool>::Ok(true);
}

Result<void> Store::audit(const std::string& op, const std::string& actor, const std::string& target,
                          const std::string& result, const std::string& request_id, const std::string& details_json) {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("INSERT INTO audit_log(at_us, op, actor, target, result, request_id, details_json)"
                          " VALUES(?, ?, ?, ?, ?, ?, ?)");
    if (!st.ok())
        return Result<void>::Err(st.error());
    st.value().bind_int64(1, now_unix_us());
    st.value().bind_text(2, op);
    st.value().bind_text(3, actor);
    st.value().bind_text(4, target);
    st.value().bind_text(5, result);
    st.value().bind_text(6, request_id);
    st.value().bind_text(7, details_json.empty() ? "{}" : details_json);
    auto step = st.value().step();
    if (!step.ok() || step.value() != Stmt::Step::Done) {
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "audit insert failed"));
    }
    return Result<void>::Ok();
}

Result<std::string> Store::integrity_check() {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("PRAGMA integrity_check");
    if (!st.ok())
        return Result<std::string>::Err(st.error());
    std::string out;
    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return Result<std::string>::Err(step.error());
        if (step.value() != Stmt::Step::Row)
            break;
        if (!out.empty())
            out += "; ";
        out += st.value().column_text(0);
    }
    return Result<std::string>::Ok(out);
}

Result<int64_t> Store::foreign_key_violations() {
    std::lock_guard<std::mutex> lock(mu_);
    auto st = db_.prepare("PRAGMA foreign_key_check");
    if (!st.ok())
        return Result<int64_t>::Err(st.error());
    int64_t violations = 0;
    for (;;) {
        auto step = st.value().step();
        if (!step.ok())
            return Result<int64_t>::Err(step.error());
        if (step.value() != Stmt::Step::Row)
            break;
        ++violations;
    }
    return Result<int64_t>::Ok(violations);
}

Result<int> Store::schema_version() {
    std::lock_guard<std::mutex> lock(mu_);
    auto rc = current_version(db_);
    if (!rc.ok())
        return Result<int>::Err(rc.error());
    return Result<int>::Ok(rc.value());
}

} // namespace storage
} // namespace streamforge
