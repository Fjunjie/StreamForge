#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "streamforge/ingest/pipeline.hpp"
#include "streamforge/processing/derived.hpp"
#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using namespace streamforge::processing;

namespace {

constexpr int64_t SEC = 1000000LL;

NormalizedSample sample(const std::string& metric, int64_t t_us, double value, bool is_null = false) {
    NormalizedSample s;
    s.device_id = "dev-01";
    s.metric_id = metric;
    s.event_time_us = t_us;
    s.ingest_time_us = t_us;
    s.value = value;
    s.value_is_null = is_null;
    s.quality = 2;
    return s;
}

ConfigSnapshot config_with(const std::vector<DerivedMetricCfg>& dms) {
    ConfigSnapshot cs;
    MetricCfg a;
    a.id = "a";
    a.canonical_unit = "x";
    a.input_units = {"x"};
    MetricCfg b;
    b.id = "b";
    b.canonical_unit = "y";
    b.input_units = {"y"};
    cs.cfg.metrics = {a, b};
    cs.cfg.derived_metrics = dms;
    return cs;
}

const DerivedCalculator::Output* find(const std::vector<DerivedCalculator::Output>& outs,
                                      const std::string& metric) {
    for (const auto& o : outs)
        if (o.metric_id == metric)
            return &o;
    return nullptr;
}

// Unwraps a successful create() (DerivedCalculator is move-only).
DerivedCalculator take_calc(const ConfigSnapshot& cs) {
    auto r = DerivedCalculator::create(cs);
    REQUIRE(r.ok());
    return r.take();
}

} // namespace

TEST_CASE("derived compile rejects unknown references and cycles", "[derived]") {
    auto unknown = config_with({DerivedMetricCfg{"d", "u", "missing_metric + 1", "null", 0}});
    auto r1 = DerivedCalculator::create(unknown);
    REQUIRE_FALSE(r1.ok());
    CHECK(r1.error().message.find("unknown metric") != std::string::npos);

    auto cycle = config_with({DerivedMetricCfg{"d1", "u", "d2 + 1", "null", 0},
                              DerivedMetricCfg{"d2", "u", "d1 + 1", "null", 0}});
    auto r2 = DerivedCalculator::create(cycle);
    REQUIRE_FALSE(r2.ok());
    CHECK(r2.error().message.find("cycle") != std::string::npos);

    auto self = config_with({DerivedMetricCfg{"d1", "u", "d1 + 1", "null", 0}});
    CHECK_FALSE(DerivedCalculator::create(self).ok());

    auto bad_expr = config_with({DerivedMetricCfg{"d1", "u", "a +", "null", 0}});
    CHECK_FALSE(DerivedCalculator::create(bad_expr).ok());
}

TEST_CASE("derived computes event-driven outputs and derived-on-derived chains", "[derived]") {
    // d1 = a * 2, d2 = d1 + b: d2 references the derived d1.
    auto cs = config_with({DerivedMetricCfg{"d1", "u1", "a * 2", "null", 0},
                           DerivedMetricCfg{"d2", "u2", "d1 + b", "null", 0}});
    auto calc = take_calc(cs);

    auto outs = calc.observe(sample("a", 1000, 5.0));
    REQUIRE(outs.size() == 2); // both d1 and d2 triggered, topological order
    CHECK(outs[0].metric_id == "d1");
    CHECK(outs[0].value == 10.0);
    CHECK(outs[0].unit == "u1");
    CHECK_FALSE(outs[0].value_is_null);
    CHECK(outs[1].metric_id == "d2");
    CHECK(outs[1].value_is_null); // b never seen: Null propagates to a NULL output

    // Observing b triggers only d2 (its direct input), using the stored d1 value.
    auto outs2 = calc.observe(sample("b", 2000, 3.0));
    REQUIRE(outs2.size() == 1);
    CHECK(outs2[0].metric_id == "d2");
    CHECK(outs2[0].value == 13.0);

    // 'a' is a transitive input of both nodes: observing it recomputes both.
    CHECK(calc.observe(sample("a", 3000, 7.0)).size() == 2);
}

TEST_CASE("derived missing policies and input age", "[derived]") {
    // d = a + 1 computed when 'a' is missing or stale.
    auto cs = config_with({DerivedMetricCfg{"d", "u", "a + 1", "null", 0}});
    auto calc = take_calc(cs);
    // 'a' never seen -> null policy outputs NULL.
    auto outs = calc.observe(sample("a", 1000, 0.0, /*is_null=*/true));
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].metric_id == "d");
    CHECK(outs[0].value_is_null);
    CHECK(calc.compute_errors() == 0);

    // skip policy: no output when the input is unusable.
    auto cs2 = config_with({DerivedMetricCfg{"d", "u", "a + 1", "skip", 0}});
    auto calc2 = take_calc(cs2);
    CHECK(calc2.observe(sample("a", 1000, 0.0, true)).empty());

    // null policy with max_input_age: a stale input is unusable -> NULL output.
    auto cs3 = config_with({DerivedMetricCfg{"d", "u", "a + 1", "null", 5 * SEC}});
    auto calc3 = take_calc(cs3);
    REQUIRE(calc3.observe(sample("a", 1000, 10.0)).size() == 1); // fresh: computed
    auto stale = calc3.observe(sample("a", 1000 + 6 * SEC, 0.0, true)); // null reading
    REQUIRE(stale.size() == 1);
    CHECK(stale[0].value_is_null);

    // recent policy: falls back to the latest known value regardless of age.
    auto cs4 = config_with({DerivedMetricCfg{"d", "u", "a + 1", "recent", 5 * SEC}});
    auto calc4 = take_calc(cs4);
    REQUIRE(calc4.observe(sample("a", 1000, 10.0)).size() == 1);
    auto fallback = calc4.observe(sample("a", 1000 + 60 * SEC, 0.0, true));
    REQUIRE(fallback.size() == 1);
    CHECK_FALSE(fallback[0].value_is_null);
    CHECK(fallback[0].value == 11.0);
}

TEST_CASE("derived compute errors are counted and skipped, never fatal", "[derived]") {
    auto cs = config_with({DerivedMetricCfg{"d", "u", "a / b", "null", 0}});
    auto calc = take_calc(cs);

    // 'a' never seen: Null propagation yields a NULL output without evaluation.
    auto outs = calc.observe(sample("b", 1000, 0.0));
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].value_is_null);
    CHECK(calc.compute_errors() == 0);

    // 'a' present and 'b' == 0: division by zero is skipped and counted.
    CHECK(calc.observe(sample("a", 2000, 8.0)).empty());
    CHECK(calc.compute_errors() == 1);

    // 'b' fixed: the next evaluation computes normally.
    auto ok = calc.observe(sample("b", 3000, 2.0));
    REQUIRE(ok.size() == 1);
    CHECK_FALSE(ok[0].value_is_null);
    CHECK(ok[0].value == 4.0);
    CHECK(calc.compute_errors() == 1);
}

TEST_CASE("derived ignores late samples", "[derived]") {
    auto cs = config_with({DerivedMetricCfg{"d", "u", "a * 2", "null", 0}});
    auto calc = take_calc(cs);
    REQUIRE(calc.observe(sample("a", 5000, 10.0)).size() == 1);
    // An older sample of 'a' must not move the state backwards.
    CHECK(calc.observe(sample("a", 4000, 99.0)).empty());
}

TEST_CASE("pipeline persists derived samples flagged and versioned", "[pipeline][derived]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    // Attach a derived metric to the fixture config.
    ConfigSnapshot mutable_cfg = *cfg;
    DerivedMetricCfg dm;
    dm.id = "d.temp";
    dm.unit = "C2";
    dm.expression = "temp * 2";
    dm.missing_policy = "null";
    mutable_cfg.cfg.derived_metrics.push_back(dm);
    auto cfg2 = std::make_shared<const ConfigSnapshot>(mutable_cfg);

    auto store_rc = storage::Store::open(cfg2->cfg.database.path);
    REQUIRE(store_rc.ok());
    auto store = store_rc.take();
    REQUIRE(store->sync_catalog(*cfg2).ok());

    const char* header = "device_id,metric,event_time,value,unit,quality,sequence,tags\n";
    std::string rows;
    for (int i = 0; i < 3; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "2026-08-01T09:15:%02d.000Z", 10 + i);
        rows += std::string("dev-01,temp,") + buf + "," + std::to_string(10 + i) + ",C,0," +
                std::to_string(i + 1) + ",line=A\n";
    }
    auto path = dir.file("input/derived.csv", header + rows);

    std::vector<ImportPipeline::CommittedSample> committed;
    ImportPipeline pipeline(cfg2, store);
    pipeline.on_samples_committed = [&](const std::vector<ImportPipeline::CommittedSample>& batch) {
        for (const auto& c : batch)
            committed.push_back(c);
    };
    auto result = pipeline.process_file(path);
    REQUIRE(result.outcome == ImportPipeline::ProcessResult::Outcome::Completed);

    // 3 real temp samples + 3 derived d.temp samples persisted.
    CHECK(store->count_samples("dev-01", "temp", 0, 2000000000000000LL).value() == 3);
    CHECK(store->count_samples("dev-01", "d.temp", 0, 2000000000000000LL).value() == 3);

    auto q = store->query_samples({"dev-01", "d.temp", 0, 2000000000000000LL, 100, false});
    REQUIRE(q.ok());
    REQUIRE(q.value().size() == 3);
    CHECK((q.value()[0].flags & sample_flags::kDerived) != 0);
    CHECK(q.value()[0].input_unit == "C2");
    CHECK(q.value()[0].value == 20.0); // temp(10) * 2
    CHECK(q.value()[2].value == 24.0);
    CHECK(q.value()[0].config_version == static_cast<int64_t>(cfg2->version));

    // The committed-sample callback reported both real and derived samples.
    REQUIRE(committed.size() == 6);
    size_t derived_count = 0;
    for (const auto& c : committed)
        if (c.metric_id == "d.temp")
            ++derived_count;
    CHECK(derived_count == 3);
}
