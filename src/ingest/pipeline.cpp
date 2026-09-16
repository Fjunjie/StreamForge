#include "streamforge/ingest/pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "streamforge/core/fs_util.hpp"
#include "streamforge/core/log.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/core/uuid.hpp"
#include "streamforge/ingest/archive.hpp"
#include "streamforge/ingest/csv_parser.hpp"
#include "streamforge/ingest/error_report.hpp"
#include "streamforge/ingest/format.hpp"
#include "streamforge/ingest/jsonl_parser.hpp"
#include "streamforge/ingest/tlm_parser.hpp"
#include "streamforge/ingest/validation.hpp"

namespace streamforge {
namespace fs = std::filesystem;

using nlohmann::json;
using storage::FileStatus;

ImportPipeline::ImportPipeline(std::shared_ptr<const ConfigSnapshot> cs, std::shared_ptr<storage::Store> store)
    : cs_(std::move(cs)), store_(std::move(store)) {}

namespace {

std::string tags_to_json(const std::vector<std::pair<std::string, std::string>>& tags, bool leap_second) {
    json obj = json::object();
    for (const auto& t : tags) {
        obj[t.first] = t.second;
    }
    if (leap_second) {
        obj["leap_second"] = "true";
    }
    return obj.dump();
}

std::string error_summary_json(const std::string& code, const std::string& message) {
    json obj;
    obj["code"] = code;
    obj["message"] = message;
    return obj.dump();
}

} // namespace

ImportPipeline::ProcessResult ImportPipeline::process_file(const std::string& path) {
    ProcessResult result;
    fault_batch_counter_ = 0;
    auto identity = compute_file_identity(path);
    if (!identity.ok()) {
        result.error = identity.error();
        return result;
    }
    auto existing = store_->find_file_by_identity(identity.value().identity_hash);
    if (!existing.ok()) {
        result.error = existing.error();
        return result;
    }
    const auto& existing_row = existing.value();
    if (existing_row) {
        return handle_existing(*existing_row);
    }
    return start_new_file(identity.value());
}

ImportPipeline::ProcessResult ImportPipeline::handle_existing(const storage::SourceFileRow& row) {
    ProcessResult result;
    result.file_id = row.id;
    fault_batch_counter_ = 0;

    if (file_status_terminal(row.status)) {
        result.outcome = ProcessResult::Outcome::SkippedDuplicate;
        result.error = Error::make(ErrorCode::FileSkippedDuplicate,
                                   std::string("file identity already recorded as ") + file_status_name(row.status));
        return result;
    }

    // Recoverable: verify the source still matches, then resume from the checkpoint.
    auto identity = compute_file_identity(row.path);
    if (!identity.ok()) {
        store_->update_file_status(row.id, FileStatus::Missing);
        result.outcome = ProcessResult::Outcome::Failed;
        result.error = Error::make(ErrorCode::SourceMissing, "source file disappeared").ctx("path", row.path);
        return result;
    }
    if (identity.value().identity_hash != row.identity_hash) {
        store_->update_file_status(row.id, FileStatus::SourceChanged);
        result.outcome = ProcessResult::Outcome::Failed;
        result.error =
            Error::make(ErrorCode::SourceChanged, "source changed since processing started; recorded as new version")
                .ctx("path", row.path);
        return result;
    }

    auto cp = store_->get_checkpoint(row.id);
    if (!cp.ok()) {
        result.error = cp.error();
        return result;
    }
    const auto& cp_row = cp.value();
    if (row.status == FileStatus::Stage1Processing) {
        return run_stage1(row, identity.value(), true, cp_row ? cp_row->stage1_offset : 0,
                          cp_row ? cp_row->stage1_line : 0, cp_row ? cp_row->stage2_cursor : 0);
    }
    // VALIDATED / PROCESSING_STAGE2: continue stage 2.
    auto stage2 = run_stage2(row);
    if (!stage2.ok()) {
        result.outcome = ProcessResult::Outcome::Failed;
        result.error = stage2.error();
        return result;
    }
    result.outcome = ProcessResult::Outcome::Completed;
    result.accepted = row.accepted_count;
    result.total = row.record_count;
    result.format_errors = row.format_errors;
    result.business_errors = row.business_errors;
    return result;
}

ImportPipeline::ProcessResult ImportPipeline::start_new_file(const FileIdentity& identity) {
    ProcessResult result;

    // Same path but different content: the previous version must not silently stay active.
    auto latest = store_->latest_file_by_path(identity.path);
    const storage::SourceFileRow* latest_row = nullptr;
    if (latest.ok()) {
        const auto& latest_opt = latest.value();
        if (latest_opt)
            latest_row = &*latest_opt;
    }
    if (latest_row && file_status_recoverable(latest_row->status)) {
        store_->update_file_status(latest_row->id, FileStatus::SourceChanged);
        SPDLOG_LOGGER_WARN(logger("ingest"),
                           "source changed for a file still being processed; old task marked "
                           "SOURCE_CHANGED: {}",
                           identity.path);
    }

    storage::SourceFileRow row;
    row.id = uuid_v4();
    row.path = identity.path;
    row.size_bytes = identity.size_bytes;
    row.mtime_us = identity.mtime_us;
    row.sha256_64k = identity.sha256_64k;
    row.identity_hash = identity.identity_hash;
    row.status = FileStatus::Stage1Processing;
    row.config_version = static_cast<int64_t>(cs_->version);
    row.first_seen_at_us = now_unix_us();

    InputFormat fmt = format_from_extension([&] {
        std::string name = fs::path(identity.path).filename().string();
        name = strip_ready_suffix(name);
        size_t dot = name.find_last_of('.');
        return dot == std::string::npos ? std::string() : name.substr(dot + 1);
    }());
    if (fmt == InputFormat::Unknown) {
        result.error = Error::make(ErrorCode::FormatUnsupported, "unsupported input format").ctx("path", identity.path);
        return result;
    }
    row.format = format_name(fmt);

    auto ins = store_->insert_source_file(row);
    if (!ins.ok()) {
        result.error = ins.error();
        return result;
    }
    return run_stage1(row, identity, false, 0, 0, 0);
}

ImportPipeline::ProcessResult ImportPipeline::run_stage1(const storage::SourceFileRow& row,
                                                         const FileIdentity& identity, bool resume,
                                                         int64_t resume_offset, int64_t resume_line_base,
                                                         int64_t resume_tlm_sequence) {
    ProcessResult result;
    result.file_id = row.id;

    std::ifstream in(identity.path, std::ios::binary);
    if (!in) {
        result.error = Error::make(ErrorCode::IoOpen, "cannot open input file").ctx("path", identity.path);
        return result;
    }

    Stage1Context ctx;
    // Continue from the counters persisted with the last committed batch so the absolute
    // counter UPDATE in stage_batch does not wipe pre-crash statistics (resume consistency).
    ctx.counts.record_count = row.record_count;
    ctx.counts.accepted_count = row.accepted_count;
    ctx.counts.format_errors = row.format_errors;
    ctx.counts.business_errors = row.business_errors;
    ctx.cp.stage = 1;
    ctx.cp.stage1_offset = resume ? resume_offset : 0;
    ctx.cp.stage1_line = resume ? resume_line_base : 0;

    TimePointUs ingest_start = now_time();
    const int64_t batch_size = cs_->cfg.pipeline.batch_size;
    std::vector<storage::StagedRecord> batch;

    auto flush = [&](bool final_flush) -> ProcessResult::Outcome {
        if (batch.empty() && !final_flush)
            return ProcessResult::Outcome::Completed;
        if (fault_injector && fault_injector(++fault_batch_counter_)) {
            return ProcessResult::Outcome::Interrupted; // simulated crash before commit
        }
        auto rc = store_->stage_batch(row.id, batch, ctx.cp, ctx.counts);
        batch.clear();
        if (!rc.ok())
            return ProcessResult::Outcome::Failed;
        return ProcessResult::Outcome::Completed;
    };

    if (row.format == format_name(InputFormat::Csv)) {
        CsvParser parser(in, CsvOptions{});
        if (resume && resume_offset > 0) {
            // The checkpoint sits past the header: re-consume it from the start, then jump.
            auto header = parser.consume_header();
            if (header.kind == CsvParser::Next::Kind::Fatal) {
                auto finalized = store_->finalize_quarantine(
                    row.id, error_summary_json(header.error.code_name(), header.error.message));
                if (!finalized.ok()) {
                    // DB state is authoritative: do not move the file while the DB update
                    // failed; the failure propagates and the file stays resumable.
                    result.error = finalized.error();
                    return result;
                }
                return quarantine_now(row, header.error.code_name(),
                                      "file could not be parsed: " + header.error.message, ctx);
            }
            in.clear();
            in.seekg(resume_offset);
            if (!in) {
                result.error = Error::make(ErrorCode::IoRead, "cannot seek to checkpoint")
                                   .ctx("path", identity.path)
                                   .ctx("offset", std::to_string(resume_offset));
                return result;
            }
            parser.seek_to(resume_offset);
        }
        while (true) {
            auto next = parser.next();
            if (next.kind == CsvParser::Next::Kind::Eof)
                break;
            if (next.kind == CsvParser::Next::Kind::Fatal) {
                auto finalized =
                    store_->finalize_quarantine(row.id, error_summary_json(next.error.code_name(), next.error.message));
                if (!finalized.ok()) {
                    result.error = finalized.error();
                    return result;
                }
                return quarantine_now(row, next.error.code_name(), "file could not be parsed: " + next.error.message,
                                      ctx);
            }
            if (should_stop && should_stop()) {
                result.outcome = ProcessResult::Outcome::Interrupted;
                return result;
            }
            storage::StagedRecord staged;
            staged.ingest_time_us = to_unix_us(ingest_start);
            staged.position = next.record.position;
            staged.line_no = resume_line_base + next.record.line_no;
            if (next.kind == CsvParser::Next::Kind::SkipRecord) {
                staged.status = StagedStatus::FormatError;
                staged.error_code = next.error.code_value();
                staged.error_message = next.error.message;
                ctx.counts.record_count++;
                ctx.counts.format_errors++;
                batch.push_back(std::move(staged));
            } else {
                auto outcome = validate_record(next.record, *cs_, ingest_start);
                ctx.counts.record_count++;
                if (outcome.kind == ValidationKind::Accepted) {
                    staged.status = StagedStatus::Accepted;
                    staged.device_id = next.record.device_id;
                    staged.metric = next.record.metric;
                    staged.has_event_time = true;
                    staged.event_time_us = to_unix_us(outcome.event_time);
                    staged.has_value = next.record.has_value;
                    staged.value_is_null = next.record.value_is_null;
                    staged.value = next.record.value;
                    staged.unit = next.record.unit;
                    staged.quality = next.record.has_quality ? next.record.quality : 0;
                    staged.has_sequence = next.record.has_sequence;
                    staged.sequence = next.record.sequence;
                    staged.tags_json = tags_to_json(next.record.tags, outcome.leap_second);
                    staged.ext_json = next.record.ext_json;
                    ctx.counts.accepted_count++;
                } else {
                    staged.status = outcome.kind == ValidationKind::BusinessError ? StagedStatus::BusinessError
                                                                                  : StagedStatus::FormatError;
                    staged.error_code = outcome.error.code_value();
                    staged.error_message = outcome.error.message;
                    if (outcome.kind == ValidationKind::BusinessError) {
                        ctx.counts.business_errors++;
                    } else {
                        ctx.counts.format_errors++;
                    }
                    if (static_cast<int64_t>(ctx.sample_errors.size()) < 100) {
                        SampleError se;
                        se.line_no = staged.line_no;
                        se.code = outcome.error.code_name();
                        se.message = outcome.error.message;
                        se.raw = next.record.raw_prefix;
                        ctx.sample_errors.push_back(std::move(se));
                    }
                }
                batch.push_back(std::move(staged));
            }

            ctx.cp.stage1_offset = parser.current_offset();
            ctx.cp.stage1_line = resume_line_base + next.record.line_no;
            if (static_cast<int64_t>(batch.size()) >= batch_size) {
                auto flushed = flush(false);
                if (flushed == ProcessResult::Outcome::Interrupted) {
                    result.outcome = ProcessResult::Outcome::Interrupted;
                    return result;
                }
                if (flushed == ProcessResult::Outcome::Failed) {
                    result.error = Error::make(ErrorCode::DbExec, "staging batch commit failed");
                    return result;
                }
            }
        }
    } else if (row.format == format_name(InputFormat::Jsonl)) {
        JsonlParser parser(in);
        if (resume && resume_offset > 0) {
            in.clear();
            in.seekg(resume_offset);
            if (!in) {
                result.error = Error::make(ErrorCode::IoRead, "cannot seek to checkpoint")
                                   .ctx("path", identity.path)
                                   .ctx("offset", std::to_string(resume_offset));
                return result;
            }
            parser.seek_to(resume_offset);
        }
        while (true) {
            auto next = parser.next();
            if (next.kind == JsonlParser::Next::Kind::Eof)
                break;
            if (should_stop && should_stop()) {
                result.outcome = ProcessResult::Outcome::Interrupted;
                return result;
            }
            storage::StagedRecord staged;
            staged.ingest_time_us = to_unix_us(ingest_start);
            staged.line_no = resume_line_base + next.record.line_no;
            staged.position = next.record.position;
            if (next.kind == JsonlParser::Next::Kind::SkipRecord) {
                staged.status = StagedStatus::FormatError;
                staged.error_code = next.error.code_value();
                staged.error_message = next.error.message;
                ctx.counts.record_count++;
                ctx.counts.format_errors++;
                batch.push_back(std::move(staged));
            } else {
                auto outcome = validate_record(next.record, *cs_, ingest_start);
                ctx.counts.record_count++;
                if (outcome.kind == ValidationKind::Accepted) {
                    staged.status = StagedStatus::Accepted;
                    staged.device_id = next.record.device_id;
                    staged.metric = next.record.metric;
                    staged.has_event_time = true;
                    staged.event_time_us = to_unix_us(outcome.event_time);
                    staged.has_value = next.record.has_value;
                    staged.value_is_null = next.record.value_is_null;
                    staged.value = next.record.value;
                    staged.unit = next.record.unit;
                    staged.quality = next.record.has_quality ? next.record.quality : 0;
                    staged.has_sequence = next.record.has_sequence;
                    staged.sequence = next.record.sequence;
                    staged.tags_json = tags_to_json(next.record.tags, outcome.leap_second);
                    staged.ext_json = next.record.ext_json;
                    ctx.counts.accepted_count++;
                } else {
                    staged.status = outcome.kind == ValidationKind::BusinessError ? StagedStatus::BusinessError
                                                                                  : StagedStatus::FormatError;
                    staged.error_code = outcome.error.code_value();
                    staged.error_message = outcome.error.message;
                    if (outcome.kind == ValidationKind::BusinessError) {
                        ctx.counts.business_errors++;
                    } else {
                        ctx.counts.format_errors++;
                    }
                    if (static_cast<int64_t>(ctx.sample_errors.size()) < 100) {
                        SampleError se;
                        se.line_no = staged.line_no;
                        se.code = outcome.error.code_name();
                        se.message = outcome.error.message;
                        se.raw = next.record.raw_prefix;
                        ctx.sample_errors.push_back(std::move(se));
                    }
                }
                batch.push_back(std::move(staged));
            }
            ctx.cp.stage1_offset = parser.current_offset();
            ctx.cp.stage1_line = resume_line_base + next.record.line_no;
            if (static_cast<int64_t>(batch.size()) >= batch_size) {
                auto flushed = flush(false);
                if (flushed == ProcessResult::Outcome::Interrupted) {
                    result.outcome = ProcessResult::Outcome::Interrupted;
                    return result;
                }
                if (flushed == ProcessResult::Outcome::Failed) {
                    result.error = Error::make(ErrorCode::DbExec, "staging batch commit failed");
                    return result;
                }
            }
        }
    } else { // TLM (M2): frames decoded via the C11 codec; resync marks records suspicious.
        TlmParser parser(in);
        if (resume && resume_offset > 0) {
            in.clear();
            in.seekg(resume_offset);
            if (!in) {
                result.error = Error::make(ErrorCode::IoRead, "cannot seek to checkpoint")
                                   .ctx("path", identity.path)
                                   .ctx("offset", std::to_string(resume_offset));
                return result;
            }
            parser.seek_to(resume_offset);
            // The last accepted frame sequence is persisted in the checkpoint's stage2_cursor
            // column while the file is in stage 1.
            if (resume_tlm_sequence > 0)
                parser.set_last_sequence(static_cast<uint64_t>(resume_tlm_sequence));
        }
        while (true) {
            auto next = parser.next();
            if (next.kind == TlmParser::Next::Kind::Eof)
                break;
            if (should_stop && should_stop()) {
                result.outcome = ProcessResult::Outcome::Interrupted;
                return result;
            }
            storage::StagedRecord staged;
            staged.ingest_time_us = to_unix_us(ingest_start);
            staged.line_no = resume_line_base + next.record.position; // frame offset as line no
            staged.position = next.record.position;
            if (next.kind == TlmParser::Next::Kind::Fatal) {
                // Invalid file header: quarantine like the CSV bad-header path.
                auto finalized =
                    store_->finalize_quarantine(row.id, error_summary_json(next.error.code_name(), next.error.message));
                if (!finalized.ok()) {
                    result.error = finalized.error();
                    return result;
                }
                return quarantine_now(row, next.error.code_name(), "file could not be parsed: " + next.error.message,
                                      ctx);
            }
            if (next.kind == TlmParser::Next::Kind::SkipRecord) {
                staged.status = StagedStatus::FormatError;
                staged.error_code = next.error.code_value();
                staged.error_message = next.error.message;
                ctx.counts.record_count++;
                ctx.counts.format_errors++;
                batch.push_back(std::move(staged));
            } else {
                auto outcome = validate_record(next.record, *cs_, ingest_start);
                ctx.counts.record_count++;
                if (outcome.kind == ValidationKind::Accepted) {
                    staged.status = StagedStatus::Accepted;
                    staged.device_id = next.record.device_id;
                    staged.metric = next.record.metric;
                    staged.has_event_time = true;
                    staged.event_time_us = to_unix_us(outcome.event_time);
                    staged.has_value = next.record.has_value;
                    staged.value_is_null = next.record.value_is_null;
                    staged.value = next.record.value;
                    staged.unit = next.record.unit;
                    staged.quality = next.record.has_quality ? next.record.quality : 0;
                    staged.has_sequence = next.record.has_sequence;
                    staged.sequence = next.record.sequence;
                    staged.tags_json = tags_to_json(next.record.tags, outcome.leap_second);
                    ctx.counts.accepted_count++;
                } else {
                    staged.status = outcome.kind == ValidationKind::BusinessError ? StagedStatus::BusinessError
                                                                                  : StagedStatus::FormatError;
                    staged.error_code = outcome.error.code_value();
                    staged.error_message = outcome.error.message;
                    if (outcome.kind == ValidationKind::BusinessError) {
                        ctx.counts.business_errors++;
                    } else {
                        ctx.counts.format_errors++;
                    }
                    if (static_cast<int64_t>(ctx.sample_errors.size()) < 100) {
                        SampleError se;
                        se.line_no = staged.line_no;
                        se.code = outcome.error.code_name();
                        se.message = outcome.error.message;
                        se.raw = next.record.raw_prefix;
                        ctx.sample_errors.push_back(std::move(se));
                    }
                }
                batch.push_back(std::move(staged));
            }
            ctx.cp.stage1_offset = parser.current_offset();
            ctx.cp.stage2_cursor = parser.has_last_sequence() ? static_cast<int64_t>(parser.last_sequence()) : 0;
            if (static_cast<int64_t>(batch.size()) >= batch_size) {
                auto flushed = flush(false);
                if (flushed == ProcessResult::Outcome::Interrupted) {
                    result.outcome = ProcessResult::Outcome::Interrupted;
                    return result;
                }
                if (flushed == ProcessResult::Outcome::Failed) {
                    result.error = Error::make(ErrorCode::DbExec, "staging batch commit failed");
                    return result;
                }
            }
        }
    }

    // Flush the final partial batch.
    {
        auto flushed = flush(true);
        if (flushed == ProcessResult::Outcome::Interrupted) {
            result.outcome = ProcessResult::Outcome::Interrupted;
            return result;
        }
        if (flushed == ProcessResult::Outcome::Failed) {
            result.error = Error::make(ErrorCode::DbExec, "final staging batch commit failed");
            return result;
        }
    }

    // Error-rate decision (agreed design: file-level staging, decision at EOF).
    int64_t denom = std::max<int64_t>(ctx.counts.record_count, cs_->cfg.pipeline.error_rate_min_records);
    double error_rate = ctx.counts.record_count == 0
                            ? 0.0
                            : static_cast<double>(ctx.counts.business_errors) / static_cast<double>(denom);
    SPDLOG_LOGGER_INFO(logger("ingest"), "file {}: records={} business_errors={} error_rate={} threshold={}", row.path,
                       ctx.counts.record_count, ctx.counts.business_errors, error_rate,
                       cs_->cfg.pipeline.max_error_rate);
    result.total = ctx.counts.record_count;
    result.accepted = ctx.counts.accepted_count;
    result.format_errors = ctx.counts.format_errors;
    result.business_errors = ctx.counts.business_errors;

    if (error_rate > cs_->cfg.pipeline.max_error_rate) {
        std::string summary = error_summary_json(
            "ValidationErrorRateExceeded", "business error rate " + std::to_string(error_rate) + " exceeds threshold " +
                                               std::to_string(cs_->cfg.pipeline.max_error_rate));
        auto rc = store_->finalize_quarantine(row.id, summary);
        if (!rc.ok()) {
            result.error = rc.error();
            return result;
        }
        return quarantine_now(row, "ValidationErrorRateExceeded", "business error rate above threshold", ctx);
    }

    auto validated = store_->update_file_status(row.id, FileStatus::Validated);
    if (!validated.ok()) {
        result.error = validated.error();
        return result;
    }

    auto stage2 = run_stage2(row);
    if (!stage2.ok()) {
        result.outcome = ProcessResult::Outcome::Failed;
        result.error = stage2.error();
        return result;
    }
    result.outcome = ProcessResult::Outcome::Completed;
    return result;
}

ImportPipeline::ProcessResult ImportPipeline::quarantine_now(const storage::SourceFileRow& row,
                                                             const std::string& error_code, const std::string& summary,
                                                             const Stage1Context& ctx) {
    ProcessResult result;
    result.file_id = row.id;
    result.outcome = ProcessResult::Outcome::Quarantined;
    result.total = ctx.counts.record_count;
    result.accepted = ctx.counts.accepted_count;
    result.format_errors = ctx.counts.format_errors;
    result.business_errors = ctx.counts.business_errors;

    int64_t denom = std::max<int64_t>(ctx.counts.record_count, cs_->cfg.pipeline.error_rate_min_records);
    double rate = ctx.counts.record_count == 0
                      ? 0.0
                      : static_cast<double>(ctx.counts.business_errors) / static_cast<double>(denom);
    std::string report =
        build_error_report(row.path, row.identity_hash, error_code, summary, ctx.counts.record_count,
                           ctx.counts.business_errors, ctx.counts.format_errors, rate, cs_->version, ctx.sample_errors);
    auto moved = quarantine_file(row.path, cs_->cfg.directories.quarantine, row.identity_hash, report);
    if (!moved.ok()) {
        // The DB state is authoritative; the physical move is retried by the operator or the
        // next recovery pass. Log loudly.
        SPDLOG_LOGGER_ERROR(logger("ingest"), "quarantine move failed for {}: {}", row.path, moved.error().message);
        result.error = moved.error();
    }
    return result;
}

Result<ImportPipeline::RecoveryStats> ImportPipeline::recover_pending() {
    RecoveryStats stats;
    auto pending =
        store_->files_in_status({FileStatus::Stage1Processing, FileStatus::Validated, FileStatus::Stage2Processing});
    if (!pending.ok())
        return Result<RecoveryStats>::Err(pending.error());
    for (const auto& row : pending.value()) {
        auto result = handle_existing(row);
        switch (result.outcome) {
        case ProcessResult::Outcome::Completed:
            ++stats.resumed;
            break;
        case ProcessResult::Outcome::SkippedDuplicate:
            break;
        case ProcessResult::Outcome::Quarantined:
            ++stats.resumed;
            break;
        default:
            if (result.error.code == ErrorCode::SourceMissing) {
                ++stats.missing;
            } else if (result.error.code == ErrorCode::SourceChanged) {
                ++stats.source_changed;
            }
            break;
        }
    }

    // Complete pending archive moves (FR-REC-002 step 5).
    auto completed = store_->files_in_status({FileStatus::Completed});
    if (!completed.ok())
        return Result<RecoveryStats>::Err(completed.error());
    for (const auto& row : completed.value()) {
        std::error_code ec;
        if (!fs::exists(fs::path(row.path), ec))
            continue;
        auto moved = archive_file(row.path, cs_->cfg.directories.archive, row.identity_hash);
        if (moved.ok()) {
            ++stats.archived;
        } else {
            SPDLOG_LOGGER_ERROR(logger("ingest"), "archive catch-up failed for {}: {}", row.path,
                                moved.error().message);
        }
    }

    // Quarantined files whose physical move was interrupted: move them now with a report
    // rebuilt from the stored error summary.
    auto quarantined = store_->files_in_status({FileStatus::Quarantined});
    if (!quarantined.ok())
        return Result<RecoveryStats>::Err(quarantined.error());
    for (const auto& row : quarantined.value()) {
        std::error_code ec;
        if (!fs::exists(fs::path(row.path), ec))
            continue;
        std::string report = build_error_report(row.path, row.identity_hash, "Quarantined", row.error_summary,
                                                row.record_count, row.business_errors, row.format_errors,
                                                static_cast<double>(row.business_errors) /
                                                    static_cast<double>(std::max<int64_t>(row.record_count, 1)),
                                                static_cast<uint64_t>(row.config_version), {});
        auto moved = quarantine_file(row.path, cs_->cfg.directories.quarantine, row.identity_hash, report);
        if (moved.ok()) {
            ++stats.archived;
        } else {
            SPDLOG_LOGGER_ERROR(logger("ingest"), "quarantine catch-up failed for {}: {}", row.path,
                                moved.error().message);
        }
    }
    return Result<RecoveryStats>::Ok(stats);
}

// ---------------------------------------------------------------------------
// Stage 2 (M2): the formal processing chain
// ---------------------------------------------------------------------------

// Rebuilds the in-memory processing context for one file. All durable state lives in the
// database, so a crash at any point is healed by replaying from the stage-2 cursor.
void ImportPipeline::init_stage2_context(const storage::SourceFileRow& row) {
    (void)row;
    dedup_ = std::make_unique<processing::DedupIndex>(*store_);
    windows_ = std::make_unique<processing::WindowAggregator>(*cs_, *store_);
    emitted_ = std::make_unique<std::vector<processing::NormalizedSample>>();
    synth_buffer_ = std::make_unique<std::vector<processing::NormalizedSample>>();
    interval_buffer_ = std::make_unique<std::vector<processing::GapDetector::MissingInterval>>();
    reorder_ = std::make_unique<processing::ReorderBuffer>(
        cs_->cfg.pipeline.allowed_lateness_us, 100000, 64u * 1024u * 1024u,
        [this](processing::NormalizedSample&& s) { emitted_->push_back(std::move(s)); });
    gaps_ = std::make_unique<processing::GapDetector>(
        *cs_, [this](processing::NormalizedSample&& s) { synth_buffer_->push_back(std::move(s)); },
        [this](const processing::GapDetector::MissingInterval& iv) { interval_buffer_->push_back(iv); });
    seeded_devices_.clear();
}

// Seeds the device watermark from durable history so cross-file lateness is detected:
// a backfilled file older than the stream gets kLate flags and respects closed windows.
void ImportPipeline::ensure_device_seeded(const std::string& device_id) {
    if (seeded_devices_.count(device_id))
        return;
    seeded_devices_.insert(device_id);
    auto max_us = store_->max_event_time_for_device(device_id);
    if (max_us.ok()) {
        const auto& max_opt = max_us.value();
        if (max_opt)
            reorder_->seed_device(device_id, *max_opt);
    }
}

// Normalizes one staged record: calibration -> unit conversion -> validity range
// (requirements FR-VAL-003/FR-VAL-004). Null values pass through untouched.
Result<processing::NormalizedSample> ImportPipeline::normalize_staged(const storage::StagedRecord& r) const {
    processing::NormalizedSample s;
    s.device_id = r.device_id;
    s.metric_id = r.metric;
    s.event_time_us = r.event_time_us;
    s.ingest_time_us = r.ingest_time_us;
    s.quality = r.quality;
    s.has_sequence = r.has_sequence;
    s.sequence = r.sequence;
    s.tags_json = r.tags_json;
    s.input_unit = r.unit;
    s.source_position = r.line_no;
    if (r.value_is_null) {
        s.value_is_null = true;
        return Result<processing::NormalizedSample>::Ok(std::move(s));
    }
    const MetricCfg* metric = cs_->metric(r.metric);
    if (metric == nullptr) {
        return Result<processing::NormalizedSample>::Err(
            Error::make(ErrorCode::ValidationMetricUnknown, "metric missing from configuration")
                .ctx("metric", r.metric));
    }
    double calibrated = r.value;
    auto cal = streamforge::processing::apply_calibration(*cs_, r.device_id, r.metric, r.value, &calibrated);
    if (!cal.ok())
        return Result<processing::NormalizedSample>::Err(cal.error());
    auto conv = core_units::convert_unit(r.unit, metric->canonical_unit, calibrated);
    if (!conv.ok())
        return Result<processing::NormalizedSample>::Err(conv.error());
    double canonical = conv.value();
    if ((metric->valid_min && canonical < *metric->valid_min) ||
        (metric->valid_max && canonical > *metric->valid_max)) {
        return Result<processing::NormalizedSample>::Err(
            Error::make(ErrorCode::ValidationValueInvalid, "value outside the configured valid range")
                .ctx("metric", r.metric)
                .ctx("value", std::to_string(canonical)));
    }
    s.value = canonical;
    return Result<processing::NormalizedSample>::Ok(std::move(s));
}

// Processes one normalized sample: dedup pre-check, sample row for the current batch and
// window registration (closed windows are skipped; only correction may touch them).
Result<void> ImportPipeline::process_emitted(const storage::SourceFileRow& file, processing::NormalizedSample& s,
                                             bool synthetic, std::vector<storage::SampleRow>& out) {
    if (synthetic)
        s.flags |= sample_flags::kSynthetic;
    windows_->on_sample(s);
    if (!synthetic) {
        auto dup = dedup_->is_duplicate(s);
        if (!dup.ok())
            return Result<void>::Err(dup.error());
        if (dup.value())
            return Result<void>::Ok();
    }
    storage::SampleRow out_row;
    out_row.sample_uuid = uuid_v4();
    out_row.device_id = s.device_id;
    out_row.metric_id = s.metric_id;
    out_row.event_time_us = s.event_time_us;
    out_row.ingest_time_us = s.ingest_time_us;
    out_row.value_is_null = s.value_is_null;
    out_row.value = s.value;
    out_row.input_unit = s.input_unit; // provenance: the pre-conversion unit
    out_row.quality = s.quality;
    out_row.source_file_id = file.id;
    out_row.source_position = s.source_position;
    out_row.has_sequence = s.has_sequence;
    out_row.sequence = s.sequence;
    out_row.flags = s.flags;
    out_row.config_version = static_cast<int64_t>(cs_->version);
    out_row.tags_json = s.tags_json;
    out_row.normalized_value = processing::normalized_value_text(s.value_is_null, s.value);
    out.push_back(std::move(out_row));
    dedup_->mark_seen(s);
    return Result<void>::Ok();
}

Result<void> ImportPipeline::run_stage2(const storage::SourceFileRow& row) {
    auto cp = store_->get_checkpoint(row.id);
    if (!cp.ok())
        return Result<void>::Err(cp.error());
    const auto& cp_row = cp.value();
    int64_t cursor = cp_row ? cp_row->stage2_cursor : 0;
    const int64_t batch_size = cs_->cfg.pipeline.batch_size;

    init_stage2_context(row);
    std::vector<processing::NormalizedSample> late_corrections;
    std::vector<SampleError> stage2_errors;
    int64_t stage2_dropped = 0;

    while (true) {
        auto rows = store_->load_staging(row.id, cursor, batch_size);
        if (!rows.ok())
            return Result<void>::Err(rows.error());
        if (rows.value().empty())
            break;
        if (should_stop && should_stop()) {
            return Result<void>::Err(Error::make(ErrorCode::Interrupted, "stage 2 interrupted"));
        }

        std::vector<storage::SampleRow> samples;
        std::set<std::string> touched_devices;
        for (const auto& staged : rows.value()) {
            if (staged.status != StagedStatus::Accepted)
                continue;
            ensure_device_seeded(staged.device_id);
            touched_devices.insert(staged.device_id);

            auto normalized = normalize_staged(staged);
            if (!normalized.ok()) {
                // Calibration/conversion/range rejections count as stage-2 business errors.
                ++stage2_dropped;
                if (static_cast<int64_t>(stage2_errors.size()) < 100) {
                    SampleError se;
                    se.line_no = staged.line_no;
                    se.code = normalized.error().code_name();
                    se.message = normalized.error().message;
                    se.raw = sanitize_raw(staged.tags_json, 64);
                    stage2_errors.push_back(std::move(se));
                }
                SPDLOG_LOGGER_DEBUG(logger("ingest"), "stage 2 dropped record: {}", normalized.error().message);
                continue;
            }
            reorder_->push(std::move(normalized.value()));

            // Drain everything the reorder buffer released (ordered per device); late
            // corrections run after the batch commit so windows read committed data.
            for (auto& s : *emitted_) {
                auto rc = process_emitted(row, s, false, samples);
                if (!rc.ok())
                    return Result<void>::Err(rc.error());
                if ((s.flags & sample_flags::kLate) != 0)
                    late_corrections.push_back(s);
                gaps_->feed(s);
            }
            emitted_->clear();
            // Synthetic interpolation samples join the same batch; the gap that produced
            // them is already closed by their real endpoints (no gap feed).
            for (auto& s : *synth_buffer_) {
                auto rc = process_emitted(row, s, true, samples);
                if (!rc.ok())
                    return Result<void>::Err(rc.error());
            }
            synth_buffer_->clear();
            for (const auto& iv : *interval_buffer_) {
                auto ins = store_->insert_missing_interval(iv.device_id, iv.metric_id, iv.start_us, iv.end_us,
                                                           iv.expected_count);
                if (!ins.ok())
                    return Result<void>::Err(ins.error());
            }
            interval_buffer_->clear();
        }

        // Before the cursor may advance, every buffered sample must be durable: drain the
        // reorder buffers (end-of-batch drain, no forced_flush flag) so "cursor advanced"
        // always implies "samples committed". This keeps the buffer bounded per batch and
        // makes crash recovery exact (replay starts from the committed cursor).
        for (const auto& device : touched_devices) {
            reorder_->flush_device(device, /*mark_forced=*/false);
            for (auto& s : *emitted_) {
                auto rc = process_emitted(row, s, false, samples);
                if (!rc.ok())
                    return Result<void>::Err(rc.error());
                if ((s.flags & sample_flags::kLate) != 0)
                    late_corrections.push_back(s);
                gaps_->feed(s);
            }
            emitted_->clear();
            for (auto& s : *synth_buffer_) {
                auto rc = process_emitted(row, s, true, samples);
                if (!rc.ok())
                    return Result<void>::Err(rc.error());
            }
            synth_buffer_->clear();
            for (const auto& iv : *interval_buffer_) {
                auto ins = store_->insert_missing_interval(iv.device_id, iv.metric_id, iv.start_us, iv.end_us,
                                                           iv.expected_count);
                if (!ins.ok())
                    return Result<void>::Err(ins.error());
            }
            interval_buffer_->clear();
        }

        if (fault_injector && fault_injector(++fault_batch_counter_)) {
            return Result<void>::Err(Error::make(ErrorCode::Interrupted, "simulated crash"));
        }

        int64_t max_seq = rows.value().back().seq;
        auto rc = store_->commit_stage2_batch(row.id, samples, max_seq);
        if (!rc.ok())
            return Result<void>::Err(rc.error());
        cursor = max_seq;

        // Durable now: advance window closing to the watermark, apply late corrections.
        for (const auto& device : touched_devices) {
            auto wm = reorder_->watermark(device);
            if (wm != INT64_MIN) {
                auto closed = windows_->on_watermark(device, wm);
                if (!closed.ok())
                    return Result<void>::Err(closed.error());
            }
        }
        for (const auto& s : late_corrections) {
            auto corrected = windows_->correct_for_sample(s);
            if (!corrected.ok())
                return Result<void>::Err(corrected.error());
        }
        late_corrections.clear();
    }

    // End of file: drain the reorder buffers (regular end-of-stream, no forced_flush
    // flag), commit the tail, then close every window this file touched. Window closing
    // recomputes from the samples table, so it is idempotent and crash-safe.
    std::vector<storage::SampleRow> tail_samples;
    for (const auto& device : seeded_devices_) {
        reorder_->flush_device(device, /*mark_forced=*/false);
        for (auto& s : *emitted_) {
            auto rc = process_emitted(row, s, false, tail_samples);
            if (!rc.ok())
                return Result<void>::Err(rc.error());
            gaps_->feed(s);
        }
        emitted_->clear();
        for (auto& s : *synth_buffer_) {
            auto rc = process_emitted(row, s, true, tail_samples);
            if (!rc.ok())
                return Result<void>::Err(rc.error());
        }
        synth_buffer_->clear();
        for (const auto& iv : *interval_buffer_) {
            auto ins =
                store_->insert_missing_interval(iv.device_id, iv.metric_id, iv.start_us, iv.end_us, iv.expected_count);
            if (!ins.ok())
                return Result<void>::Err(ins.error());
        }
        interval_buffer_->clear();
    }
    if (!tail_samples.empty()) {
        auto rc = store_->insert_samples(tail_samples);
        if (!rc.ok())
            return Result<void>::Err(rc.error());
        tail_samples.clear();
    }
    for (const auto& device : seeded_devices_) {
        auto wm = reorder_->watermark(device);
        if (wm != INT64_MIN) {
            auto closed = windows_->on_watermark(device, wm);
            if (!closed.ok())
                return Result<void>::Err(closed.error());
        }
        auto all = windows_->close_all(device);
        if (!all.ok())
            return Result<void>::Err(all.error());
    }

    if (stage2_dropped > 0) {
        SPDLOG_LOGGER_INFO(logger("ingest"), "file {}: stage 2 dropped {} records", row.path, stage2_dropped);
    }
    if (stage2_dropped > 0) {
        // Stage-2 rejections shrink the accepted count and grow the business error count.
        auto counts = storage::FileCounts{row.record_count, row.accepted_count - stage2_dropped, row.format_errors,
                                          row.business_errors + stage2_dropped};
        auto updated = store_->update_file_counts(row.id, counts);
        if (!updated.ok())
            return Result<void>::Err(updated.error());
    }

    auto done = store_->finalize_completed(row.id);
    if (!done.ok())
        return Result<void>::Err(done.error());

    // Archive after the transaction; a crash here is healed by recover_pending().
    auto moved = archive_file(row.path, cs_->cfg.directories.archive, row.identity_hash);
    if (!moved.ok()) {
        SPDLOG_LOGGER_ERROR(logger("ingest"), "archive move failed for {}: {}", row.path, moved.error().message);
    }
    return Result<void>::Ok();
}
} // namespace streamforge
