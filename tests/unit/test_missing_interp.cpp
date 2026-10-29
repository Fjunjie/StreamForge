#include <catch2/catch_test_macros.hpp>

#include "streamforge/processing/missing.hpp"
#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using streamforge::processing::GapDetector;

using streamforge::processing::NormalizedSample;

namespace {
struct Sink {
    std::vector<NormalizedSample> synthetic;
    std::vector<streamforge::processing::GapDetector::MissingInterval> intervals;
    GapDetector make(const ConfigSnapshot& cs) {
        return {cs, [this](NormalizedSample&& s) { synthetic.push_back(std::move(s)); },
                [this](const streamforge::processing::GapDetector::MissingInterval& iv) { intervals.push_back(iv); }};
    }
};

NormalizedSample valid(const std::string& device, const std::string& metric, int64_t t, double v) {
    NormalizedSample s;
    s.device_id = device;
    s.metric_id = metric;
    s.event_time_us = t;
    s.ingest_time_us = t;
    s.value = v;
    s.quality = 0;
    return s;
}
} // namespace

TEST_CASE("gap detection produces intervals with expected counts", "[missing]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    // temp: expected_period 10s (set below), jitter 0, no interpolation.
    auto modified = std::const_pointer_cast<ConfigSnapshot>(cfg);
    modified->cfg.metrics[0].expected_period_us = 10'000'000;
    modified->cfg.metrics[0].jitter_us = 0;
    modified->cfg.metrics[0].interpolation = Interpolation::None;

    Sink sink;
    auto gaps = sink.make(*modified);
    gaps.feed(valid("dev-01", "temp", 100'000'000, 1.0));
    gaps.feed(valid("dev-01", "temp", 110'000'000, 2.0)); // exactly one period: no gap
    CHECK(sink.intervals.empty());
    gaps.feed(valid("dev-01", "temp", 150'000'000, 3.0)); // 40s gap: 3 missing points

    REQUIRE(sink.intervals.size() == 1);
    CHECK(sink.intervals[0].start_us == 110'000'000);
    CHECK(sink.intervals[0].end_us == 150'000'000);
    CHECK(sink.intervals[0].expected_count == 3);
    CHECK(sink.synthetic.empty());
}

TEST_CASE("interpolation respects policy, max_gap and endpoints", "[missing]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto modified = std::const_pointer_cast<ConfigSnapshot>(cfg);
    modified->cfg.metrics[0].expected_period_us = 10'000'000;
    modified->cfg.metrics[0].jitter_us = 0;
    modified->cfg.metrics[0].max_gap_us = 35'000'000; // gaps <= 35s interpolate
    modified->cfg.metrics[0].interpolation = Interpolation::Linear;

    Sink sink;
    auto gaps = sink.make(*modified);
    gaps.feed(valid("dev-01", "temp", 100'000'000, 10.0));
    gaps.feed(valid("dev-01", "temp", 130'000'000, 40.0)); // 30s gap: 2 interior points

    REQUIRE(sink.intervals.size() == 1);
    REQUIRE(sink.synthetic.size() == 2);
    CHECK(sink.synthetic[0].event_time_us == 110'000'000);
    CHECK(sink.synthetic[0].value == 20.0);
    CHECK((sink.synthetic[0].flags & sample_flags::kSynthetic) != 0);
    CHECK(sink.synthetic[1].event_time_us == 120'000'000);
    CHECK(sink.synthetic[1].value == 30.0);

    // Beyond max_gap: interval recorded, no interpolation.
    gaps.feed(valid("dev-01", "temp", 200'000'000, 50.0)); // 70s gap > 35s
    REQUIRE(sink.intervals.size() == 2);
    CHECK(sink.synthetic.size() == 2);
}

TEST_CASE("previous-value interpolation fills with the last value", "[missing]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto modified = std::const_pointer_cast<ConfigSnapshot>(cfg);
    modified->cfg.metrics[0].expected_period_us = 10'000'000;
    modified->cfg.metrics[0].max_gap_us = 60'000'000;
    modified->cfg.metrics[0].interpolation = Interpolation::Previous;

    Sink sink;
    auto gaps = sink.make(*modified);
    gaps.feed(valid("dev-01", "temp", 100'000'000, 7.0));
    gaps.feed(valid("dev-01", "temp", 120'000'000, 9.0));
    REQUIRE(sink.synthetic.size() == 1);
    CHECK(sink.synthetic[0].value == 7.0);
}

TEST_CASE("device-error samples are never interpolation endpoints", "[missing]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto modified = std::const_pointer_cast<ConfigSnapshot>(cfg);
    modified->cfg.metrics[0].expected_period_us = 10'000'000;
    modified->cfg.metrics[0].max_gap_us = 60'000'000;
    modified->cfg.metrics[0].interpolation = Interpolation::Linear;

    Sink sink;
    auto gaps = sink.make(*modified);
    gaps.feed(valid("dev-01", "temp", 100'000'000, 1.0));
    auto bad = valid("dev-01", "temp", 110'000'000, 99.0);
    bad.quality = 2; // device error: invisible to gap detection
    gaps.feed(bad);
    gaps.feed(valid("dev-01", "temp", 120'000'000, 3.0)); // gap 100->120 with error between

    // The error sample did not split the gap: one interval across it, endpoints 1.0/3.0.
    REQUIRE(sink.intervals.size() == 1);
    CHECK(sink.intervals[0].start_us == 100'000'000);
    CHECK(sink.intervals[0].end_us == 120'000'000);
    REQUIRE(sink.synthetic.size() == 1);
    CHECK(sink.synthetic[0].event_time_us == 110'000'000);
    CHECK(sink.synthetic[0].value == 2.0);
}

TEST_CASE("metrics without an expected period are not tracked", "[missing]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path); // pressure has no expected_period
    Sink sink;
    auto gaps = sink.make(*cfg);
    gaps.feed(valid("dev-01", "pressure", 100'000'000, 1.0));
    gaps.feed(valid("dev-01", "pressure", 900'000'000, 2.0));
    CHECK(sink.intervals.empty());
    CHECK(sink.synthetic.empty());
}

TEST_CASE("seed_from_store restores the last valid state after a crash", "[missing]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto modified = std::const_pointer_cast<ConfigSnapshot>(cfg);
    modified->cfg.metrics[0].expected_period_us = 10'000'000;
    auto store_rc = storage::Store::open(modified->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*modified).ok());

    std::vector<storage::SampleRow> rows;
    storage::SampleRow r;
    r.sample_uuid = "s1";
    r.device_id = "dev-01";
    r.metric_id = "temp";
    r.event_time_us = 500'000'000;
    r.ingest_time_us = 500'000'000;
    r.value = 3.0;
    r.quality = 0;
    r.config_version = 1;
    rows.push_back(r);
    REQUIRE(store->insert_samples(rows).ok());

    Sink sink;
    auto gaps = sink.make(*modified);
    REQUIRE(gaps.seed_from_store(*store, "dev-01", "temp").value());
    // Next sample 20s later: gap across the seeded state produces one interval.
    gaps.feed(valid("dev-01", "temp", 520'000'000, 4.0));
    REQUIRE(sink.intervals.size() == 1);
    CHECK(sink.intervals[0].start_us == 500'000'000);
}
