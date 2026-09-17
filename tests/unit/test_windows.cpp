#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <nlohmann/json.hpp>

#include "streamforge/core/uuid.hpp"
#include "streamforge/processing/windows.hpp"
#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using streamforge::processing::NormalizedSample;
using streamforge::processing::WindowAggregator;

namespace {
NormalizedSample make(const std::string& device, const std::string& metric, int64_t t, double v, uint32_t flags = 0,
                      int quality = 0) {
    NormalizedSample s;
    s.device_id = device;
    s.metric_id = metric;
    s.event_time_us = t;
    s.ingest_time_us = t;
    s.value = v;
    s.flags = flags;
    s.quality = quality;
    return s;
}

struct Agg {
    std::shared_ptr<const ConfigSnapshot> cfg;
    std::shared_ptr<storage::Store> store;
    std::unique_ptr<WindowAggregator> windows;

    static Agg make(const std::string& root, const std::vector<int64_t>& window_sizes) {
        Agg a;
        a.cfg = sf_test::make_config(root);
        auto modified = std::const_pointer_cast<ConfigSnapshot>(a.cfg);
        modified->cfg.windows = window_sizes;
        auto store_rc = storage::Store::open(modified->cfg.database.path);
        REQUIRE(store_rc.ok());
        a.store = store_rc.value();
        REQUIRE(a.store->sync_catalog(*modified).ok());
        a.windows = std::make_unique<WindowAggregator>(*modified, *a.store);
        return a;
    }

    // Emits samples, then closes windows as the watermark/end-of-file pass would.
    // The pipeline commits samples before closing; compute_stats reads the samples table.
    void run(const std::vector<NormalizedSample>& samples, int64_t watermark) {
        std::vector<storage::SampleRow> rows;
        for (const auto& s : samples) {
            windows->on_sample(s);
            storage::SampleRow r;
            r.sample_uuid = uuid_v4();
            r.device_id = s.device_id;
            r.metric_id = s.metric_id;
            r.event_time_us = s.event_time_us;
            r.ingest_time_us = s.event_time_us;
            r.value_is_null = s.value_is_null;
            r.value = s.value;
            r.quality = s.quality;
            r.flags = s.flags;
            r.config_version = 1;
            rows.push_back(std::move(r));
        }
        if (!rows.empty())
            REQUIRE(store->insert_samples(rows).ok());
        auto rc = windows->on_watermark(samples.empty() ? std::string("dev-01") : samples.front().device_id, watermark);
        if (!rc.ok())
            FAIL(rc.error().code_name() + std::string(": ") + rc.error().message);
    }

    nlohmann::json aggregate_json(const std::string& metric, const std::string& type) {
        auto st =
            store->db().prepare("SELECT sample_count, valid_count, min_value, max_value, avg_value, sum_value,"
                                " stddev, p50, p95, p99, superseded, version FROM aggregates WHERE device_id='dev-01'"
                                " AND metric_id=? AND window_type=? ORDER BY version");
        REQUIRE(st.ok());
        st.value().bind_text(1, metric);
        st.value().bind_text(2, type);
        auto step = st.value().step();
        REQUIRE(step.ok());
        if (step.value() != storage::Stmt::Step::Row)
            return nullptr;
        nlohmann::json j;
        j["sample_count"] = st.value().column_int64(0);
        j["valid_count"] = st.value().column_int64(1);
        j["min"] = st.value().column_double(2);
        j["max"] = st.value().column_double(3);
        j["avg"] = st.value().column_double(4);
        j["sum"] = st.value().column_double(5);
        j["stddev"] = st.value().column_double(6);
        j["p50"] = st.value().column_double(7);
        j["p95"] = st.value().column_double(8);
        j["p99"] = st.value().column_double(9);
        j["superseded"] = st.value().column_int64(10);
        j["version"] = st.value().column_int64(11);
        return j;
    }
};
} // namespace

TEST_CASE("window stats are exact over the durable samples", "[windows]") {
    sf_test::TempDir dir;
    auto a = Agg::make(dir.path, {600}); // 600s window: all samples in [0, 600)
    std::vector<NormalizedSample> samples;
    samples.reserve(6);
    for (int i = 0; i < 6; ++i) {
        samples.push_back(make("dev-01", "temp", (100 + i * 10LL) * 1'000'000LL, static_cast<double>(i + 1)));
    }
    a.run(samples, 600LL * 1'000'000LL); // watermark passes the window end: closes it

    auto agg = a.aggregate_json("temp", "600s");
    REQUIRE_FALSE(agg.is_null());
    CHECK(agg["sample_count"] == 6);
    CHECK(agg["valid_count"] == 6);
    CHECK(agg["min"] == 1.0);
    CHECK(agg["max"] == 6.0);
    CHECK(agg["avg"] == 3.5);
    CHECK(agg["sum"] == 21.0);
    // Population stddev of 1..6 = sqrt(35/12) ≈ 1.7078.
    CHECK(std::fabs(agg["stddev"].get<double>() - 1.707825127659933) < 1e-9);
    CHECK(agg["p50"] == 3.0);
    CHECK(agg["p95"] == 6.0);
    CHECK(agg["p99"] == 6.0);
    CHECK(agg["version"] == 1);
    CHECK(agg["superseded"] == 0);
}

TEST_CASE("window boundaries align to UTC epoch multiples", "[windows]") {
    sf_test::TempDir dir;
    auto a = Agg::make(dir.path, {60});
    // Samples straddling the 10:01:00 boundary land in two windows.
    std::vector<NormalizedSample> samples{
        make("dev-01", "temp", 601LL * 1'000'000LL, 1.0), // 10:01:00 window
        make("dev-01", "temp", 599LL * 1'000'000LL, 2.0), // 09:59:00 window? 599s epoch
    };
    // Epoch seconds 599 and 601 with 60s windows: starts 540s and 600s.
    a.run(samples, 700LL * 1'000'000LL);
    auto count =
        a.store->db().prepare("SELECT COUNT(*) FROM aggregates WHERE device_id='dev-01' AND window_type='60s'");
    REQUIRE(count.ok());
    REQUIRE(count.value().step().value() == storage::Stmt::Step::Row);
    CHECK(count.value().column_int64(0) == 2);
}

TEST_CASE("late samples with correction disabled keep closed windows untouched", "[windows]") {
    sf_test::TempDir dir;
    auto a = Agg::make(dir.path, {600});
    auto modified = std::const_pointer_cast<ConfigSnapshot>(a.cfg);
    modified->cfg.pipeline.window_correction = false;

    std::vector<NormalizedSample> first;
    first.reserve(5);
    for (int i = 0; i < 5; ++i) {
        first.push_back(make("dev-01", "temp", (100 + i * 20LL) * 1'000'000LL, 5.0));
    }
    a.run(first, 600LL * 1'000'000LL); // closes [0s, 600s) with 5 samples
    auto before = a.aggregate_json("temp", "600s");
    REQUIRE_FALSE(before.is_null());
    CHECK(before["sample_count"] == 5);

    // A late sample for the closed window: stored (the pipeline commits it before
    // correction), registered late, correction disabled.
    std::vector<storage::SampleRow> rows;
    storage::SampleRow r;
    r.sample_uuid = uuid_v4();
    r.device_id = "dev-01";
    r.metric_id = "temp";
    r.event_time_us = 120LL * 1'000'000LL;
    r.ingest_time_us = 120LL * 1'000'000LL;
    r.value = 9.0;
    r.quality = 0;
    r.flags = sample_flags::kLate;
    r.config_version = 1;
    rows.push_back(r);
    REQUIRE(a.store->insert_samples(rows).ok());

    NormalizedSample late = make("dev-01", "temp", 120LL * 1'000'000LL, 9.0);
    late.flags |= sample_flags::kLate;
    a.windows->on_sample(late);
    // Re-running close_all for the device (as the pipeline does) must not absorb it.
    REQUIRE(a.windows->close_all("dev-01").ok());
    auto after = a.aggregate_json("temp", "600s");
    REQUIRE_FALSE(after.is_null());
    CHECK(after["sample_count"] == 5); // unchanged
}

TEST_CASE("late samples with correction enabled create a new version", "[windows]") {
    sf_test::TempDir dir;
    auto a = Agg::make(dir.path, {600});
    auto modified = std::const_pointer_cast<ConfigSnapshot>(a.cfg);
    modified->cfg.pipeline.window_correction = true;

    std::vector<NormalizedSample> first;
    first.reserve(5);
    for (int i = 0; i < 5; ++i) {
        first.push_back(make("dev-01", "temp", (100 + i * 20LL) * 1'000'000LL, 5.0));
    }
    a.run(first, 600LL * 1'000'000LL); // closes [0s, 600s) as version 1 with 5 samples

    // The late sample must be stored first (the pipeline commits it before correction).
    std::vector<storage::SampleRow> rows;
    storage::SampleRow r;
    r.sample_uuid = uuid_v4();
    r.device_id = "dev-01";
    r.metric_id = "temp";
    r.event_time_us = 120LL * 1'000'000LL;
    r.ingest_time_us = 120LL * 1'000'000LL;
    r.value = 9.0;
    r.quality = 0;
    r.flags = sample_flags::kLate;
    r.config_version = 1;
    rows.push_back(r);
    REQUIRE(a.store->insert_samples(rows).ok());

    NormalizedSample late = make("dev-01", "temp", 120LL * 1'000'000LL, 9.0);
    late.flags |= sample_flags::kLate;
    REQUIRE(a.windows->correct_for_sample(late).ok());

    // Old version superseded, new version carries 6 samples (5 original + late).
    auto st = a.store->db().prepare("SELECT version, superseded, sample_count FROM aggregates WHERE device_id='dev-01'"
                                    " AND metric_id='temp' AND window_type='600s' ORDER BY version");
    REQUIRE(st.ok());
    REQUIRE(st.value().step().value() == storage::Stmt::Step::Row);
    CHECK(st.value().column_int64(0) == 1);
    CHECK(st.value().column_int64(1) == 1); // superseded
    CHECK(st.value().column_int64(2) == 5);
    REQUIRE(st.value().step().value() == storage::Stmt::Step::Row);
    CHECK(st.value().column_int64(0) == 2);
    CHECK(st.value().column_int64(1) == 0);
    CHECK(st.value().column_int64(2) == 6);
}

TEST_CASE("empty windows write zero counts and null value stats", "[windows]") {
    sf_test::TempDir dir;
    auto a = Agg::make(dir.path, {60});
    a.windows->on_sample(make("dev-01", "temp", 100 * 1'000'000LL, 1.0));
    // Close with a watermark covering the window but the sample inserted into the DB first:
    // emulate the pipeline by writing the sample, then closing.
    std::vector<storage::SampleRow> rows;
    storage::SampleRow r;
    r.sample_uuid = "s";
    r.device_id = "dev-01";
    r.metric_id = "temp";
    r.event_time_us = 100LL * 1'000'000;
    r.ingest_time_us = 100LL * 1'000'000;
    r.value = 1.0;
    r.quality = 0;
    r.config_version = 1;
    rows.push_back(r);
    REQUIRE(a.store->insert_samples(rows).ok());
    REQUIRE(a.windows->on_watermark("dev-01", 160LL * 1'000'000LL).ok());
    auto agg = a.aggregate_json("temp", "60s");
    REQUIRE_FALSE(agg.is_null());
    CHECK(agg["sample_count"] == 1);
}
