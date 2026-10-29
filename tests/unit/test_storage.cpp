#include <catch2/catch_test_macros.hpp>

#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using namespace streamforge::storage;

TEST_CASE("migrations apply once and are idempotent", "[storage]") {
    sf_test::TempDir dir;
    auto store = Store::open(dir.path + "/t.db");
    REQUIRE(store.ok());
    CHECK(store.value()->schema_version().value() == 5);

    auto again = Store::open(dir.path + "/t.db");
    REQUIRE(again.ok());
    CHECK(again.value()->schema_version().value() == 5);
}

TEST_CASE("wal mode and foreign keys enabled", "[storage]") {
    sf_test::TempDir dir;
    auto store = Store::open(dir.path + "/t.db");
    REQUIRE(store.ok());
    auto& db = store.value()->db();
    auto st = db.prepare("PRAGMA journal_mode");
    REQUIRE(st.ok());
    REQUIRE(st.value().step().value() == Stmt::Step::Row);
    CHECK(st.value().column_text(0) == "wal");

    auto fk = db.prepare("PRAGMA foreign_keys");
    REQUIRE(fk.ok());
    REQUIRE(fk.value().step().value() == Stmt::Step::Row);
    CHECK(fk.value().column_int64(0) == 1);
}

TEST_CASE("catalog sync writes devices, metrics, units and calibrations", "[storage]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store = Store::open(dir.path + "/t.db");
    REQUIRE(store.ok());
    REQUIRE(store.value()->sync_catalog(*cfg).ok());

    auto& db = store.value()->db();
    auto count = [&](const std::string& sql) {
        auto st = db.prepare(sql);
        REQUIRE(st.ok());
        REQUIRE(st.value().step().value() == Stmt::Step::Row);
        return st.value().column_int64(0);
    };
    CHECK(count("SELECT COUNT(*) FROM devices WHERE enabled=1") == 2);
    CHECK(count("SELECT COUNT(*) FROM metrics") == 3);
    CHECK(count("SELECT COUNT(*) FROM metric_units WHERE metric_id='temp'") == 3);
    CHECK(count("SELECT COUNT(*) FROM device_metrics WHERE device_id='dev-02'") == 3);

    // A removed device becomes disabled rather than deleted.
    auto modified = std::make_shared<ConfigSnapshot>(*cfg);
    Config& c = modified->cfg;
    std::vector<DeviceCfg> keep;
    for (auto& d : c.devices) {
        if (d.id != "dev-02")
            keep.push_back(d);
    }
    c.devices = keep;
    modified->version = compute_config_version(c);
    REQUIRE(store.value()->sync_catalog(*modified).ok());
    CHECK(count("SELECT COUNT(*) FROM devices WHERE enabled=0") == 1);
}

TEST_CASE("staging batch, checkpoint and counters commit atomically", "[storage]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store = Store::open(dir.path + "/t.db");
    REQUIRE(store.ok());
    REQUIRE(store.value()->sync_catalog(*cfg).ok());

    SourceFileRow file;
    file.id = "file-1";
    file.path = dir.path + "/input/a.csv";
    file.identity_hash = "hash-1";
    file.format = "csv";
    file.config_version = 1;
    file.first_seen_at_us = 1;
    REQUIRE(store.value()->insert_source_file(file).ok());

    std::vector<StagedRecord> rows;
    for (int i = 0; i < 5; ++i) {
        StagedRecord r;
        r.position = static_cast<int64_t>(i) * 10;
        r.line_no = i + 1;
        r.ingest_time_us = 100;
        r.status = StagedStatus::Accepted;
        r.device_id = "dev-01";
        r.metric = "temp";
        r.has_event_time = true;
        r.event_time_us = 1000 + i;
        r.has_value = true;
        r.value = i;
        r.unit = "C";
        rows.push_back(r);
    }
    StagedRecord bad;
    bad.status = StagedStatus::BusinessError;
    bad.error_code = 5000;
    bad.error_message = "unknown device";
    bad.line_no = 6;
    rows.push_back(bad);

    CheckpointRow cp;
    cp.stage = 1;
    cp.stage1_offset = 60;
    cp.stage1_line = 6;
    FileCounts counts;
    counts.record_count = 6;
    counts.accepted_count = 5;
    counts.business_errors = 1;
    REQUIRE(store.value()->stage_batch("file-1", rows, cp, counts).ok());

    auto stats = store.value()->staging_stats("file-1");
    REQUIRE(stats.ok());
    CHECK(stats.value().total == 6);
    CHECK(stats.value().accepted == 5);
    CHECK(stats.value().business_errors == 1);

    auto got_cp_rc = store.value()->get_checkpoint("file-1");
    const auto& got_cp = sf_test::value_or_fail(got_cp_rc);
    CHECK(got_cp.stage1_offset == 60);

    auto loaded = store.value()->load_staging("file-1", 0, 100);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().size() == 6);
    CHECK(loaded.value().back().error_code == 5000);
}

TEST_CASE("stage2 commit inserts samples and advances cursor", "[storage]") {
    sf_test::TempDir dir;
    auto store = Store::open(dir.path + "/t.db");
    REQUIRE(store.ok());
    auto cfg = sf_test::make_config(dir.path);
    REQUIRE(store.value()->sync_catalog(*cfg).ok());

    // The checkpoint table carries a foreign key onto source_files.
    SourceFileRow file;
    file.id = "f";
    file.path = dir.path + "/input/f.csv";
    file.identity_hash = "hash-f";
    file.format = "csv";
    file.config_version = 1;
    file.first_seen_at_us = 1;
    REQUIRE(store.value()->insert_source_file(file).ok());

    std::vector<SampleRow> samples;
    for (int i = 0; i < 3; ++i) {
        SampleRow s;
        s.sample_uuid = "s" + std::to_string(i);
        s.device_id = "dev-01";
        s.metric_id = "temp";
        s.event_time_us = 1000 + i;
        s.ingest_time_us = 1000;
        s.value = 1.5 * i;
        s.input_unit = "C";
        s.source_file_id = "f";
        s.source_position = i;
        s.config_version = 1;
        samples.push_back(s);
    }
    REQUIRE(store.value()->commit_stage2_batch("f", samples, 42).ok());
    CHECK(store.value()->count_samples("dev-01", "temp", 0, 5000).value() == 3);

    auto q = store.value()->query_samples({"dev-01", "temp", 0, 5000, 10, false});
    REQUIRE(q.ok());
    REQUIRE(q.value().size() == 3);
    CHECK(q.value()[0].sample_uuid == "s0");
    CHECK(q.value()[2].value == 3.0);

    auto cp_rc = store.value()->get_checkpoint("f");
    const auto& cp = sf_test::value_or_fail(cp_rc);
    CHECK(cp.stage2_cursor == 42);

    REQUIRE(store.value()->finalize_completed("f").ok());
    auto row_rc = store.value()->find_file_by_id("f");
    const auto& row = sf_test::value_or_fail(row_rc);
    CHECK(row.status == FileStatus::Completed);
    auto stats = store.value()->staging_stats("f");
    REQUIRE(stats.ok());
    CHECK(stats.value().total == 0);
}

TEST_CASE("incident ack records history and audit", "[storage]") {
    sf_test::TempDir dir;
    auto store = Store::open(dir.path + "/t.db");
    REQUIRE(store.ok());
    auto& db = store.value()->db();
    REQUIRE(db.exec("INSERT INTO incidents(incident_uuid, rule_id, rule_version, device_id, severity,"
                    " state, started_us, config_version, created_at_us)"
                    " VALUES('inc-1', 'r1', 1, 'dev-01', 'high', 'open', 100, 1, 100)")
                .ok());

    auto acked = store.value()->ack_incident("inc-1", "operator-a", "seen", 200);
    REQUIRE(acked.ok());
    CHECK(acked.value());
    CHECK_FALSE(store.value()->ack_incident("missing", "x", "", 1).value());

    auto st = db.prepare("SELECT acked_by, ack_comment FROM incidents WHERE incident_uuid='inc-1'");
    REQUIRE(st.ok());
    REQUIRE(st.value().step().value() == Stmt::Step::Row);
    CHECK(st.value().column_text(0) == "operator-a");
    CHECK(st.value().column_text(1) == "seen");
}

TEST_CASE("integrity and foreign key checks", "[storage]") {
    sf_test::TempDir dir;
    auto store = Store::open(dir.path + "/t.db");
    REQUIRE(store.ok());
    CHECK(store.value()->integrity_check().value() == "ok");
    CHECK(store.value()->foreign_key_violations().value() == 0);

    // FK violation attempt: sample with unknown device.
    auto& db = store.value()->db();
    auto rc = db.exec("INSERT INTO samples(sample_uuid, device_id, metric_id, event_time_us, ingest_time_us,"
                      " quality, config_version) VALUES('x', 'ghost', 'temp', 1, 1, 0, 1)");
    CHECK_FALSE(rc.ok());
    CHECK(rc.error().code == ErrorCode::DbConstraint);
}
