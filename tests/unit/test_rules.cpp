#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "streamforge/rules/rule_engine.hpp"
#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using namespace streamforge::rules;
using namespace streamforge::storage;

namespace {

constexpr int64_t MIN = 60LL * 1000000;

struct Fixture {
    sf_test::TempDir dir;
    std::shared_ptr<Store> store;
    ConfigSnapshot cs;
    std::unique_ptr<RuleEngine> engine;

    Fixture() {
        auto opened = Store::open(dir.path + "/t.db");
        REQUIRE(opened.ok());
        store = opened.take();

        MetricCfg temp;
        temp.id = "temp";
        temp.canonical_unit = "C";
        temp.input_units = {"C"};
        MetricCfg pressure;
        pressure.id = "pressure";
        pressure.canonical_unit = "kPa";
        pressure.input_units = {"kPa"};
        cs.cfg.metrics = {temp, pressure};

        DeviceCfg dev1;
        dev1.id = "dev-01";
        dev1.tags = {{"line", "A"}};
        DeviceCfg dev2;
        dev2.id = "dev-02";
        dev2.tags = {{"line", "B"}};
        cs.cfg.devices = {dev1, dev2};
        cs.version = 42;

        engine = std::make_unique<RuleEngine>(*store);
    }

    static RuleCfg make_rule(const std::string& id, const std::string& trigger) {
        RuleCfg r;
        r.id = id;
        r.type = "threshold";
        r.severity = "warning";
        r.trigger = trigger;
        return r;
    }

    std::vector<IncidentRow> open_incidents() {
        auto rows = store->query_incidents("open", "", 100);
        REQUIRE(rows.ok());
        return rows.take();
    }

    std::vector<IncidentRow> closed_incidents() {
        auto rows = store->query_incidents("closed", "", 100);
        REQUIRE(rows.ok());
        return rows.take();
    }

    Result<void> eval(const std::string& device, int64_t t_us, std::map<std::string, double> values) {
        return engine->evaluate_device(device, t_us, values, {});
    }

    // Single-column scalar query helper.
    int64_t scalar(const std::string& sql, const std::string& bind_uuid = "") {
        auto st = store->db().prepare(sql);
        REQUIRE(st.ok());
        if (!bind_uuid.empty())
            REQUIRE(st.value().bind_text(1, bind_uuid).ok());
        auto step = st.value().step();
        REQUIRE(step.ok());
        REQUIRE(step.value() == Stmt::Step::Row);
        return st.value().column_int64(0);
    }
};

} // namespace

TEST_CASE("compile_rule maps type and validates metric references", "[rules]") {
    Fixture f;

    auto r = Fixture::make_rule("r1", "temp > 80");
    r.type = "duration";
    auto c = compile_rule(r, f.cs);
    REQUIRE(c.ok());
    CHECK(c.value().type == RuleType::Duration);
    REQUIRE(c.value().metric_refs.size() == 1);
    CHECK(c.value().metric_refs[0] == "temp");

    r.type = "nonsense";
    auto bad_type = compile_rule(r, f.cs);
    REQUIRE_FALSE(bad_type.ok());

    r = Fixture::make_rule("r2", "temp > 80 or bogus > 1");
    auto unknown = compile_rule(r, f.cs);
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().message.find("unknown metric") != std::string::npos);

    // Derived metrics are valid references too (ids with '-' cannot appear in
    // expressions; use dot-separated ids there).
    DerivedMetricCfg dm;
    dm.id = "d.temp";
    dm.unit = "C";
    dm.expression = "temp * 2";
    f.cs.cfg.derived_metrics.push_back(dm);
    auto with_derived = compile_rule(Fixture::make_rule("r3", "d.temp > 5"), f.cs);
    REQUIRE(with_derived.ok());
}

TEST_CASE("compile_rule records the recovery source", "[rules]") {
    Fixture f;
    auto r = Fixture::make_rule("r1", "temp > 80");
    r.recovery = "temp < 75";
    auto c = compile_rule(r, f.cs);
    REQUIRE(c.ok());
    CHECK(c.value().uses_recovery_expr);
    CHECK(c.value().recovery_expr_src == "temp < 75");
    REQUIRE(c.value().metric_refs.size() == 1); // temp deduped across trigger+recovery
}

TEST_CASE("evaluate_rule reports insufficient data instead of erroring", "[rules]") {
    Fixture f;
    auto compiled = compile_rule(Fixture::make_rule("r1", "temp > 80"), f.cs);
    REQUIRE(compiled.ok());

    expr::EvalContext no_provider;
    auto provider_missing = evaluate_rule(compiled.value(), no_provider);
    REQUIRE_FALSE(provider_missing.ok());

    expr::EvalContext no_data;
    no_data.lookup_metric = [](const std::string&) { return std::nullopt; };
    auto nodata = evaluate_rule(compiled.value(), no_data);
    REQUIRE(nodata.ok());
    CHECK_FALSE(nodata.value().has_data);
    CHECK_FALSE(nodata.value().condition_met);
    CHECK_FALSE(nodata.value().recovery_met);

    expr::EvalContext data;
    data.lookup_metric = [](const std::string& m) -> std::optional<double> {
        return m == "temp" ? std::optional<double>(85.0) : std::nullopt;
    };
    auto met = evaluate_rule(compiled.value(), data);
    REQUIRE(met.ok());
    CHECK(met.value().has_data);
    CHECK(met.value().condition_met);
}

TEST_CASE("threshold rule fires, records hits and auto-recovers", "[rules]") {
    Fixture f;
    f.cs.cfg.rules = {Fixture::make_rule("high-temp", "temp > 80")};
    REQUIRE(f.engine->load_rules(f.cs).ok());

    REQUIRE(f.eval("dev-01", 1000, {{"temp", 85.0}}).ok());
    auto open = f.open_incidents();
    REQUIRE(open.size() == 1);
    CHECK(open[0].rule_id == "high-temp");
    CHECK(open[0].device_id == "dev-01");
    CHECK(open[0].hit_count == 1);
    CHECK(open[0].started_us == 1000);
    auto uuid = open[0].uuid;

    // Still firing: hit counted, peak raised.
    REQUIRE(f.eval("dev-01", 2000, {{"temp", 90.0}}).ok());
    open = f.open_incidents();
    REQUIRE(open.size() == 1);
    CHECK(open[0].hit_count == 2);
    CHECK(f.scalar("SELECT peak_value FROM incidents WHERE incident_uuid=?", uuid) == 90);

    // Trigger false with auto-recovery closes the incident.
    REQUIRE(f.eval("dev-01", 3000, {{"temp", 70.0}}).ok());
    CHECK(f.open_incidents().empty());
    auto closed = f.closed_incidents();
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].uuid == uuid);

    // No cooldown, no merge window: a later breach opens a fresh incident.
    REQUIRE(f.eval("dev-01", 4000, {{"temp", 85.0}}).ok());
    CHECK(f.open_incidents().size() == 1);
}

TEST_CASE("duration rule waits for a sustained condition", "[rules]") {
    Fixture f;
    auto r = Fixture::make_rule("hot", "temp > 80");
    r.type = "duration";
    r.duration_us = 5 * MIN;
    f.cs.cfg.rules = {r};
    REQUIRE(f.engine->load_rules(f.cs).ok());

    REQUIRE(f.eval("dev-01", 0, {{"temp", 85.0}}).ok());
    CHECK(f.open_incidents().empty()); // pending

    REQUIRE(f.eval("dev-01", 2 * MIN, {{"temp", 85.0}}).ok());
    CHECK(f.open_incidents().empty()); // still accumulating

    REQUIRE(f.eval("dev-01", 4 * MIN, {{"temp", 70.0}}).ok()); // condition drops: reset
    REQUIRE(f.eval("dev-01", 6 * MIN, {{"temp", 85.0}}).ok()); // pending again
    REQUIRE(f.eval("dev-01", 10 * MIN, {{"temp", 85.0}}).ok()); // 4m since 6m: still pending
    CHECK(f.open_incidents().empty());

    REQUIRE(f.eval("dev-01", 11 * MIN, {{"temp", 85.0}}).ok()); // 5m elapsed: firing
    CHECK(f.open_incidents().size() == 1);
}

TEST_CASE("custom recovery requires the recovery expression", "[rules]") {
    Fixture f;
    auto r = Fixture::make_rule("hot", "temp > 80");
    r.recovery = "temp < 75";
    f.cs.cfg.rules = {r};
    REQUIRE(f.engine->load_rules(f.cs).ok());

    REQUIRE(f.eval("dev-01", 1000, {{"temp", 85.0}}).ok());
    REQUIRE(f.open_incidents().size() == 1);

    // Between trigger and recovery thresholds: hysteresis keeps the incident open.
    REQUIRE(f.eval("dev-01", 2000, {{"temp", 78.0}}).ok());
    CHECK(f.open_incidents().size() == 1);

    REQUIRE(f.eval("dev-01", 3000, {{"temp", 74.0}}).ok());
    CHECK(f.open_incidents().empty());
}

TEST_CASE("refire within the merge interval reopens the same incident", "[rules]") {
    Fixture f;
    auto r = Fixture::make_rule("flap", "temp > 80");
    r.merge_interval_us = 10 * MIN;
    f.cs.cfg.rules = {r};
    REQUIRE(f.engine->load_rules(f.cs).ok());

    REQUIRE(f.eval("dev-01", 0, {{"temp", 85.0}}).ok());
    auto first = f.open_incidents();
    REQUIRE(first.size() == 1);
    auto uuid = first[0].uuid;
    auto started = first[0].started_us;

    REQUIRE(f.eval("dev-01", MIN, {{"temp", 70.0}}).ok());
    CHECK(f.open_incidents().empty());

    REQUIRE(f.eval("dev-01", 5 * MIN, {{"temp", 85.0}}).ok()); // within the merge window
    auto open = f.open_incidents();
    REQUIRE(open.size() == 1);
    CHECK(open[0].uuid == uuid);
    CHECK(open[0].started_us == started);
    CHECK(open[0].reopen_count == 1);

    // After the merge window expires a NEW incident appears.
    REQUIRE(f.eval("dev-01", 6 * MIN, {{"temp", 70.0}}).ok());
    REQUIRE(f.eval("dev-01", 20 * MIN, {{"temp", 85.0}}).ok());
    open = f.open_incidents();
    REQUIRE(open.size() == 1);
    CHECK(open[0].uuid != uuid);
    CHECK(open[0].started_us == 20 * MIN);
}

TEST_CASE("cooldown suppresses refiring until it expires", "[rules]") {
    Fixture f;
    auto r = Fixture::make_rule("hot", "temp > 80");
    r.cooldown_us = 10 * MIN;
    f.cs.cfg.rules = {r};
    REQUIRE(f.engine->load_rules(f.cs).ok());

    REQUIRE(f.eval("dev-01", 0, {{"temp", 85.0}}).ok());
    auto first = f.open_incidents();
    REQUIRE(first.size() == 1);
    auto first_uuid = first[0].uuid;

    REQUIRE(f.eval("dev-01", MIN, {{"temp", 70.0}}).ok()); // close -> cooldown
    REQUIRE(f.eval("dev-01", 2 * MIN, {{"temp", 85.0}}).ok()); // suppressed
    CHECK(f.open_incidents().empty());

    REQUIRE(f.eval("dev-01", 11 * MIN, {{"temp", 85.0}}).ok()); // cooldown expired
    auto open = f.open_incidents();
    REQUIRE(open.size() == 1);
    CHECK(open[0].uuid != first_uuid);
}

TEST_CASE("state machine restores from the DB after a restart", "[rules]") {
    Fixture f;
    f.cs.cfg.rules = {Fixture::make_rule("hot", "temp > 80")};
    REQUIRE(f.engine->load_rules(f.cs).ok());
    REQUIRE(f.eval("dev-01", 1000, {{"temp", 85.0}}).ok());
    REQUIRE(f.open_incidents().size() == 1);
    auto uuid = f.open_incidents()[0].uuid;

    // A fresh engine over the same store simulates a restart.
    f.engine = std::make_unique<RuleEngine>(*f.store);
    REQUIRE(f.engine->load_rules(f.cs).ok());

    // Restored FIRING state recovers instead of starting over.
    REQUIRE(f.eval("dev-01", 2000, {{"temp", 70.0}}).ok());
    CHECK(f.open_incidents().empty());
    auto closed = f.closed_incidents();
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].uuid == uuid);

    // A restored PENDING timer continues from its original start.
    auto r2 = Fixture::make_rule("sustained", "temp > 80");
    r2.type = "duration";
    r2.duration_us = 5 * MIN;
    f.cs.cfg.rules = {r2};
    REQUIRE(f.engine->load_rules(f.cs).ok());
    REQUIRE(f.eval("dev-01", 10 * MIN, {{"temp", 85.0}}).ok()); // pending since 10m
    f.engine = std::make_unique<RuleEngine>(*f.store);
    REQUIRE(f.engine->load_rules(f.cs).ok());
    REQUIRE(f.eval("dev-01", 14 * MIN, {{"temp", 85.0}}).ok()); // 4m < 5m: still pending
    CHECK(f.open_incidents().empty());
    REQUIRE(f.eval("dev-01", 15 * MIN, {{"temp", 85.0}}).ok()); // 5m from original start
    CHECK(f.open_incidents().size() == 1);
}

TEST_CASE("missing data holds the machine state", "[rules]") {
    Fixture f;
    f.cs.cfg.rules = {Fixture::make_rule("hot", "temp > 80")};
    REQUIRE(f.engine->load_rules(f.cs).ok());
    REQUIRE(f.eval("dev-01", 1000, {{"temp", 85.0}}).ok());
    CHECK(f.open_incidents().size() == 1);

    // No temp value: FIRING holds — neither a spurious recovery nor an error.
    REQUIRE(f.eval("dev-01", 2000, {}).ok());
    CHECK(f.open_incidents().size() == 1);

    REQUIRE(f.eval("dev-01", 3000, {{"temp", 70.0}}).ok());
    CHECK(f.open_incidents().empty());
}

TEST_CASE("device tag selector restricts evaluation", "[rules]") {
    Fixture f;
    auto r = Fixture::make_rule("hot", "temp > 80");
    r.devices = {{"line", "A"}};
    f.cs.cfg.rules = {r};
    REQUIRE(f.engine->load_rules(f.cs).ok());

    REQUIRE(f.eval("dev-02", 1000, {{"temp", 85.0}}).ok()); // line B: not selected
    CHECK(f.open_incidents().empty());

    REQUIRE(f.eval("dev-01", 1000, {{"temp", 85.0}}).ok()); // line A: selected
    CHECK(f.open_incidents().size() == 1);
}

TEST_CASE("a failed reload keeps the previous rule set active", "[rules]") {
    Fixture f;
    f.cs.cfg.rules = {Fixture::make_rule("hot", "temp > 80")};
    REQUIRE(f.engine->load_rules(f.cs).ok());

    // One broken rule must not clear or partially replace the active set.
    f.cs.cfg.rules = {Fixture::make_rule("broken", "temp > 80 or bogus > 1")};
    auto res = f.engine->load_rules(f.cs);
    REQUIRE_FALSE(res.ok());

    REQUIRE(f.eval("dev-01", 1000, {{"temp", 85.0}}).ok());
    CHECK(f.open_incidents().size() == 1); // the ORIGINAL rule still fired
}

TEST_CASE("removing a rule closes its open incidents with RULE_REMOVED", "[rules]") {
    Fixture f;
    f.cs.cfg.rules = {Fixture::make_rule("hot", "temp > 80")};
    REQUIRE(f.engine->load_rules(f.cs).ok());
    REQUIRE(f.eval("dev-01", 1000, {{"temp", 85.0}}).ok());
    REQUIRE(f.open_incidents().size() == 1);
    auto uuid = f.open_incidents()[0].uuid;

    f.cs.cfg.rules = {};
    REQUIRE(f.engine->load_rules(f.cs).ok());
    CHECK(f.open_incidents().empty());

    auto st = f.store->db().prepare("SELECT close_reason, state FROM incidents WHERE incident_uuid=?");
    REQUIRE(st.ok());
    REQUIRE(st.value().bind_text(1, uuid).ok());
    auto step = st.value().step();
    REQUIRE(step.ok());
    REQUIRE(step.value() == Stmt::Step::Row);
    CHECK(st.value().column_text(0) == "RULE_REMOVED");
    CHECK(st.value().column_text(1) == "closed");
}

TEST_CASE("window functions in rules skip the rule without failing the pipeline", "[rules]") {
    Fixture f;
    f.cs.cfg.rules = {Fixture::make_rule("r", "RATE(temp, 5m) > 1")};
    REQUIRE(f.engine->load_rules(f.cs).ok());

    auto v = f.eval("dev-01", 1000, {{"temp", 85.0}});
    REQUIRE(v.ok()); // logged and skipped, not an error
    CHECK(f.open_incidents().empty());
}

TEST_CASE("incidents carry the config version", "[rules]") {
    Fixture f;
    f.cs.cfg.rules = {Fixture::make_rule("hot", "temp > 80")};
    REQUIRE(f.engine->load_rules(f.cs).ok());
    REQUIRE(f.eval("dev-01", 1000, {{"temp", 85.0}}).ok());
    CHECK(f.open_incidents().size() == 1);
    CHECK(f.scalar("SELECT config_version FROM incidents LIMIT 1") == 42);
}
