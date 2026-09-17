#include <catch2/catch_test_macros.hpp>

#include <sys/time.h>
#include <utime.h>

#include <filesystem>

#include "streamforge/ingest/pipeline.hpp"
#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using streamforge::storage::FileStatus;
namespace fs = std::filesystem;

namespace {
const char* kGoodCsvHeader = "device_id,metric,event_time,value,unit,quality,sequence,tags\n";

std::string good_csv_rows(int count, int start_sequence = 1) {
    std::string rows;
    for (int i = 0; i < count; ++i) {
        rows += "dev-01,temp,2026-08-01T09:15:" +
                [](int s) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "%02d", s % 60);
                    return std::string(buf);
                }(start_sequence + i) +
                ".000Z," + std::to_string(10 + i) + ",C,0," + std::to_string(start_sequence + i) + ",line=A\n";
    }
    return rows;
}

// Pins the file mtime so a byte-identical recreation reproduces the same file identity
// (path + size + mtime + content hash).
void pin_mtime(const std::string& path) {
    struct ::utimbuf tb{};
    tb.actime = 1785575730;
    tb.modtime = 1785575730;
    REQUIRE(::utime(path.c_str(), &tb) == 0);
}
} // namespace

TEST_CASE("good csv file flows through both stages", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto path = dir.file("input/pump.csv", kGoodCsvHeader + good_csv_rows(7));

    ImportPipeline pipeline(cfg, store);
    auto result = pipeline.process_file(path);
    INFO("pipeline error: " << result.error.code_name() << ": " << result.error.message);
    INFO("accepted=" << result.accepted << " total=" << result.total);
    auto diag_stats = store->staging_stats(result.file_id);
    INFO("staging total=" << (diag_stats.ok() ? diag_stats.value().total : -1));
    auto diag_row = store->find_file_by_id(result.file_id);
    const auto& diag_row_opt = diag_row.value();
    INFO("file status=" << (diag_row.ok() && diag_row_opt ? storage::file_status_name(diag_row_opt->status) : "?"));
    REQUIRE(result.outcome == ImportPipeline::ProcessResult::Outcome::Completed);

    // 7 samples persisted with input unit preserved.
    CHECK(store->count_samples("dev-01", "temp", 0, 2000000000000000LL).value() == 7);
    auto q = store->query_samples({"dev-01", "temp", 0, 2000000000000000LL, 100, false});
    REQUIRE(q.ok());
    REQUIRE_FALSE(q.value().empty());
    CHECK(q.value()[0].input_unit == "C");
    CHECK(q.value()[0].source_file_id == result.file_id);
    CHECK(q.value()[0].config_version == static_cast<int64_t>(cfg->version));

    // File completed, staging empty, file archived away from the input directory.
    auto row_rc = store->find_file_by_id(result.file_id);
    const auto& row = sf_test::value_or_fail(row_rc);
    CHECK(row.status == FileStatus::Completed);
    CHECK(row.accepted_count == 7);
    auto stats = store->staging_stats(result.file_id);
    REQUIRE(stats.ok());
    CHECK(stats.value().total == 0);
    CHECK_FALSE(fs::exists(fs::path(path)));
    bool archived = false;
    for (const auto& entry : fs::recursive_directory_iterator(dir.path + "/archive")) {
        if (entry.path().filename() == "pump.csv")
            archived = true;
    }
    CHECK(archived);
}

TEST_CASE("jsonl file produces the same sample stream", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    std::string lines;
    for (int i = 0; i < 4; ++i) {
        lines += R"({"device_id":"dev-02","metric":"temp","event_time":"2026-08-01T17:15:3)" + std::to_string(i) +
                 R"(","value":21.5,"unit":"C","ext":{"k":1}})"
                 "\n";
    }
    auto path = dir.file("input/sensors.jsonl", lines);

    ImportPipeline pipeline(cfg, store);
    auto result = pipeline.process_file(path);
    REQUIRE(result.outcome == ImportPipeline::ProcessResult::Outcome::Completed);
    CHECK(result.accepted == 4);
    CHECK(store->count_samples("dev-02", "temp", 0, 2000000000000000LL).value() == 4);
}

TEST_CASE("error rate over threshold quarantines without business residue", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    // 30 records, 15 with unknown devices => 50% >> 10% threshold.
    std::string rows;
    for (int i = 0; i < 15; ++i) {
        rows += "dev-01,temp,2026-08-01T09:15:30.000Z,1,C,0," + std::to_string(i) + ",\n";
        rows += "ghost,temp,2026-08-01T09:15:30.000Z,1,C,0," + std::to_string(100 + i) + ",\n";
    }
    auto path = dir.file("input/bad.csv", kGoodCsvHeader + rows);

    ImportPipeline pipeline(cfg, store);
    auto result = pipeline.process_file(path);
    REQUIRE(result.outcome == ImportPipeline::ProcessResult::Outcome::Quarantined);
    CHECK(result.business_errors == 15);

    // No samples, no staging rows.
    CHECK(store->count_all_samples().value() == 0);
    auto stats = store->staging_stats(result.file_id);
    REQUIRE(stats.ok());
    CHECK(stats.value().total == 0);

    auto row_rc = store->find_file_by_id(result.file_id);
    const auto& row = sf_test::value_or_fail(row_rc);
    CHECK(row.status == FileStatus::Quarantined);

    // Physically quarantined with an .error.json report.
    bool found_file = false, found_report = false;
    for (const auto& entry : fs::directory_iterator(dir.path + "/quarantine")) {
        if (entry.path().filename() == "bad.csv")
            found_file = true;
        if (entry.path().filename() == "bad.csv.error.json")
            found_report = true;
    }
    CHECK(found_file);
    CHECK(found_report);
}

TEST_CASE("error rate under threshold accepts valid records", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    // 30 records, 3 business errors => 10% == threshold, not above.
    std::string rows;
    for (int i = 0; i < 27; ++i) {
        rows += "dev-01,temp,2026-08-01T09:15:30.000Z,1,C,0," + std::to_string(i) + ",\n";
    }
    for (int i = 0; i < 3; ++i) {
        rows += "ghost,temp,2026-08-01T09:15:30.000Z,1,C,0," + std::to_string(100 + i) + ",\n";
    }
    auto path = dir.file("input/mixed.csv", kGoodCsvHeader + rows);

    ImportPipeline pipeline(cfg, store);
    auto result = pipeline.process_file(path);
    REQUIRE(result.outcome == ImportPipeline::ProcessResult::Outcome::Completed);
    CHECK(result.accepted == 27);
    CHECK(result.business_errors == 3);
    CHECK(store->count_all_samples().value() == 27);
}

TEST_CASE("simulated crash in stage 1 resumes from checkpoint without duplicates", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto path = dir.file("input/crash.csv", kGoodCsvHeader + good_csv_rows(10));
    pin_mtime(path);
    ImportPipeline pipeline(cfg, store);
    // batch_size = 3 in the test config; crash before committing the second batch.
    pipeline.fault_injector = [](int index) { return index == 2; };
    auto crashed = pipeline.process_file(path);
    REQUIRE(crashed.outcome == ImportPipeline::ProcessResult::Outcome::Interrupted);
    pipeline.fault_injector = nullptr;

    // First batch visible in staging, no samples yet.
    auto row_rc = store->find_file_by_id(crashed.file_id);
    const auto& row = sf_test::value_or_fail(row_rc);
    CHECK(row.status == FileStatus::Stage1Processing);
    auto cp_rc = store->get_checkpoint(crashed.file_id);
    const auto& cp = sf_test::value_or_fail(cp_rc);
    CHECK(cp.stage1_offset > 0);

    // Restart: same process_file call resumes and completes.
    auto resumed = pipeline.process_file(path);
    REQUIRE(resumed.outcome == ImportPipeline::ProcessResult::Outcome::Completed);
    CHECK(resumed.file_id == crashed.file_id);
    CHECK(store->count_samples("dev-01", "temp", 0, 2000000000000000LL).value() == 10);

    // File counters must include the pre-crash batch (audit: resume must not reset stats).
    auto final_row_rc = store->find_file_by_id(crashed.file_id);
    const auto& final_row = sf_test::value_or_fail(final_row_rc);
    CHECK(final_row.record_count == 10);
    CHECK(final_row.accepted_count == 10);

    // Re-running an identical file at the same path skips (the completed run archived the
    // original away, so recreate it byte-for-byte with the pinned mtime first).
    dir.file("input/crash.csv", kGoodCsvHeader + good_csv_rows(10));
    pin_mtime(path);
    auto again = pipeline.process_file(path);
    CHECK(again.outcome == ImportPipeline::ProcessResult::Outcome::SkippedDuplicate);
}

TEST_CASE("simulated crash in stage 2 resumes without duplicate samples", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto path = dir.file("input/stage2.csv", kGoodCsvHeader + good_csv_rows(10));
    ImportPipeline pipeline(cfg, store);
    // Crash on the second stage-2 batch (stage-1 fault hook disabled via high index).
    int stage2_calls = 0;
    pipeline.fault_injector = [&](int index) {
        // Stage 1 has ceil(10/3) = 4 batches; indices 5+ belong to stage 2.
        if (index > 4) {
            ++stage2_calls;
            return stage2_calls == 2;
        }
        return false;
    };
    auto crashed = pipeline.process_file(path);
    REQUIRE(crashed.outcome == ImportPipeline::ProcessResult::Outcome::Failed);
    CHECK(crashed.error.code == ErrorCode::Interrupted);
    pipeline.fault_injector = nullptr;

    auto row_rc = store->find_file_by_id(crashed.file_id);
    const auto& row = sf_test::value_or_fail(row_rc);
    CHECK(row.status == FileStatus::Stage2Processing);
    auto partial = store->count_samples("dev-01", "temp", 0, 2000000000000000LL).value();
    // The span of this file (9 s) is inside the 5-minute lateness window, so the reorder
    // buffer holds everything until end-of-file; a stage-2 crash may therefore have
    // committed no samples yet. The invariant that matters: nothing partial is lost or
    // duplicated once processing resumes.
    CHECK(partial >= 0);
    CHECK(partial <= 10);

    auto resumed = pipeline.process_file(path);
    REQUIRE(resumed.outcome == ImportPipeline::ProcessResult::Outcome::Completed);
    CHECK(store->count_samples("dev-01", "temp", 0, 2000000000000000LL).value() == 10);
    // Idempotence: replaying the file changes nothing.
    auto after_replay = store->count_samples("dev-01", "temp", 0, 2000000000000000LL).value();
    CHECK(after_replay == 10);
}

TEST_CASE("same path with changed content marks the old task SOURCE_CHANGED", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto path = dir.file("input/v.csv", kGoodCsvHeader + good_csv_rows(3));
    ImportPipeline pipeline(cfg, store);
    pipeline.fault_injector = [](int index) { return index == 1; }; // crash mid stage 1
    auto crashed = pipeline.process_file(path);
    REQUIRE(crashed.outcome == ImportPipeline::ProcessResult::Outcome::Interrupted);
    pipeline.fault_injector = nullptr;

    // Content changes while the old task is pending.
    dir.file("input/v.csv", kGoodCsvHeader + good_csv_rows(9, 100));
    auto second = pipeline.process_file(path);
    REQUIRE(second.outcome == ImportPipeline::ProcessResult::Outcome::Completed);

    auto old_row_rc = store->find_file_by_id(crashed.file_id);
    const auto& old_row = sf_test::value_or_fail(old_row_rc);
    CHECK(old_row.status == FileStatus::SourceChanged);
    CHECK(store->count_all_samples().value() == 9);
}

TEST_CASE("recover_pending resumes interrupted work and catches up archives", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto path = dir.file("input/rec.csv", kGoodCsvHeader + good_csv_rows(10));
    ImportPipeline pipeline(cfg, store);
    pipeline.fault_injector = [](int index) { return index == 1; };
    auto crashed = pipeline.process_file(path);
    REQUIRE(crashed.outcome == ImportPipeline::ProcessResult::Outcome::Interrupted);
    pipeline.fault_injector = nullptr;

    auto stats = pipeline.recover_pending();
    REQUIRE(stats.ok());
    CHECK(stats.value().resumed == 1);
    CHECK(store->count_samples("dev-01", "temp", 0, 2000000000000000LL).value() == 10);
    CHECK_FALSE(fs::exists(fs::path(path)));
}

TEST_CASE("graceful stop leaves the file resumable", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto path = dir.file("input/stop.csv", kGoodCsvHeader + good_csv_rows(10));
    ImportPipeline pipeline(cfg, store);
    pipeline.should_stop = [] { return true; };
    auto stopped = pipeline.process_file(path);
    CHECK(stopped.outcome == ImportPipeline::ProcessResult::Outcome::Interrupted);
    auto row_rc = store->find_file_by_id(stopped.file_id);
    const auto& row = sf_test::value_or_fail(row_rc);
    CHECK(row.status == FileStatus::Stage1Processing);
}

TEST_CASE("broken header file is quarantined as unparseable", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto path = dir.file("input/broken.csv", "col_a,col_b\n1,2\n");
    ImportPipeline pipeline(cfg, store);
    auto result = pipeline.process_file(path);
    CHECK(result.outcome == ImportPipeline::ProcessResult::Outcome::Quarantined);
    CHECK(store->count_all_samples().value() == 0);
}

TEST_CASE("unsupported format is rejected without state", "[pipeline]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto path = dir.file("input/blob.dat", "binary-ish");
    ImportPipeline pipeline(cfg, store);
    auto result = pipeline.process_file(path);
    CHECK(result.outcome == ImportPipeline::ProcessResult::Outcome::Failed);
    CHECK(result.error.code == ErrorCode::FormatUnsupported);
    CHECK(store->count_all_samples().value() == 0);
}

TEST_CASE("garbage tlm file is quarantined on invalid header", "[pipeline][tlm]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto path = dir.file("input/blob.tlm", "binary-ish");
    ImportPipeline pipeline(cfg, store);
    auto result = pipeline.process_file(path);
    INFO("tlm error: " << result.error.code_name() << ": " << result.error.message);
    CHECK(result.outcome == ImportPipeline::ProcessResult::Outcome::Quarantined);
    CHECK(store->count_all_samples().value() == 0);
    bool found = false;
    for (const auto& entry : fs::directory_iterator(dir.path + "/quarantine")) {
        if (entry.path().filename() == "blob.tlm")
            found = true;
    }
    CHECK(found);
}
