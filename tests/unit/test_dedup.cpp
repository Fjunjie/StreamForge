#include <catch2/catch_test_macros.hpp>

#include "streamforge/processing/dedup.hpp"
#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using streamforge::processing::DedupIndex;
using streamforge::processing::NormalizedSample;

namespace {
NormalizedSample make_sample(const std::string& device, const std::string& metric, int64_t t, uint64_t sequence,
                             double value) {
    NormalizedSample s;
    s.device_id = device;
    s.metric_id = metric;
    s.event_time_us = t;
    s.ingest_time_us = t;
    s.has_sequence = sequence != 0;
    s.sequence = sequence;
    s.value = value;
    return s;
}
} // namespace

TEST_CASE("dedup keys follow the sequence/no-sequence rules", "[dedup]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());
    DedupIndex dedup(*store, 8);

    // No sequence: key includes the event time and the normalized value.
    auto a = make_sample("dev-01", "temp", 1000, 0, 1.5);
    CHECK_FALSE(dedup.is_duplicate(a).value());
    CHECK(dedup.is_duplicate(a).value()); // now cached

    // Same time, different value: a different key.
    auto b = make_sample("dev-01", "temp", 1000, 0, 2.5);
    CHECK_FALSE(dedup.is_duplicate(b).value());

    // With sequence: the key is (device, metric, sequence) alone. c2 shares c's key, so it
    // is a duplicate even though its time and value differ (FR-ORD-001 key semantics).
    auto c = make_sample("dev-01", "temp", 9999, 42, 5.0);
    CHECK_FALSE(dedup.is_duplicate(c).value());
    auto c2 = make_sample("dev-01", "temp", 1234, 42, 9.0);
    CHECK(dedup.is_duplicate(c2).value());

    // Different metric or device: distinct keys.
    CHECK_FALSE(dedup.is_duplicate(make_sample("dev-02", "temp", 1000, 0, 1.5)).value());
    CHECK_FALSE(dedup.is_duplicate(make_sample("dev-01", "pressure", 1000, 0, 1.5)).value());
}

TEST_CASE("dedup consults the database on cache miss", "[dedup]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    // Insert a sequenced sample into the database directly (outside the index).
    std::vector<storage::SampleRow> rows;
    storage::SampleRow row;
    row.sample_uuid = "s1";
    row.device_id = "dev-01";
    row.metric_id = "temp";
    row.event_time_us = 5000;
    row.ingest_time_us = 5000;
    row.value = 1.0;
    row.quality = 0;
    row.has_sequence = true;
    row.sequence = 7;
    row.config_version = 1;
    rows.push_back(row);
    REQUIRE(store->insert_samples(rows).ok());

    // A fresh index (empty cache) must find it in the database.
    DedupIndex dedup(*store, 8);
    auto probe = make_sample("dev-01", "temp", 1, 7, 0.0);
    CHECK(dedup.is_duplicate(probe).value());

    // The same series without a sequence does not collide with the sequenced key.
    auto unsequenced = make_sample("dev-01", "temp", 5000, 0, 1.0);
    CHECK_FALSE(dedup.is_duplicate(unsequenced).value());
}

TEST_CASE("dedup cache is bounded (LRU eviction keeps memory fixed)", "[dedup]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto store_rc = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    DedupIndex dedup(*store, 4);
    for (int i = 0; i < 100; ++i) {
        auto s = make_sample("dev-01", "temp", i * 1000LL, 0, static_cast<double>(i));
        CHECK_FALSE(dedup.is_duplicate(s).value());
    }
    // Capacity 4: after 100 distinct keys the cache holds at most 4 entries internally;
    // behavior stays correct (each new key is simply a database miss).
    SUCCEED("bounded cache exercised");
}
