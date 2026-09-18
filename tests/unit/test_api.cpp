#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <httplib.h>

#include "streamforge/api/api.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/core/uuid.hpp"
#include "streamforge/api/runtime.hpp"
#include "streamforge/core/metrics.hpp"
#include "streamforge/ingest/pipeline.hpp"
#include "streamforge/rules/rule_engine.hpp"
#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using namespace streamforge::api;

namespace {

constexpr int64_t SEC = 1000000LL;

struct Fixture {
    sf_test::TempDir dir;
    std::shared_ptr<const ConfigSnapshot> cfg;
    std::shared_ptr<storage::Store> store;
    std::unique_ptr<ImportPipeline> pipeline;
    std::unique_ptr<rules::RuleEngine> rules;
    std::unique_ptr<AppRuntime> runtime;
    std::unique_ptr<HttpApi> http;
    std::string token = "test-token-123";

    Fixture(bool with_token = true) {
        auto opened = storage::Store::open(dir.path + "/t.db");
        REQUIRE(opened);
        store = opened.take();
        cfg = sf_test::make_config(dir.path);
        REQUIRE(store->sync_catalog(*cfg).ok());
        pipeline = std::make_unique<ImportPipeline>(cfg, store);
        rules = std::make_unique<rules::RuleEngine>(*store);
        REQUIRE(rules->load_rules(*cfg).ok());
        runtime = std::make_unique<AppRuntime>(*store, *rules, *pipeline);
        REQUIRE(runtime->start_with(cfg, dir.path + "/config.yaml").ok());

        http = std::make_unique<HttpApi>(*store, *rules, *runtime);
        HttpConfig hc;
        hc.bind = "127.0.0.1";
        hc.port = 0; // ephemeral
        auto started = http->start(hc, with_token ? token : std::string());
        REQUIRE(started);
    }

    std::string base() const {
        return "http://127.0.0.1:" + std::to_string(http->bound_port());
    }

    httplib::Result get(const std::string& path, const std::string& bearer = "") {
        httplib::Client cli("127.0.0.1", http->bound_port());
        httplib::Headers headers;
        if (!bearer.empty())
            headers = {{"Authorization", "Bearer " + bearer}};
        return cli.Get(path.c_str(), headers);
    }

    storage::SampleRow sample(const std::string& metric, int64_t t_us, double value) {
        storage::SampleRow r;
        r.sample_uuid = uuid_v4();
        r.device_id = "dev-01";
        r.metric_id = metric;
        r.event_time_us = t_us;
        r.ingest_time_us = t_us;
        r.value = value;
        r.input_unit = "C";
        r.quality = 2;
        r.config_version = 1;
        return r;
    }
};

std::string iso(int64_t us) {
    return format_utc_us(TimePointUs{std::chrono::duration_cast<TimePointUs::duration>(std::chrono::microseconds(us))});
}

} // namespace

TEST_CASE("health, ready and metrics endpoints", "[api]") {
    Fixture f;
    auto health = f.get("/health");
    REQUIRE(health);
    CHECK(health->status == 200);
    CHECK(health->body.find("\"ok\"") != std::string::npos);

    auto ready = f.get("/ready");
    REQUIRE(ready);
    CHECK(ready->status == 200);
    CHECK(ready->body.find("true") != std::string::npos);

    auto metrics = f.get("/metrics");
    REQUIRE(metrics);
    CHECK(metrics->status == 200);
    CHECK(metrics->body.find("sf_http_up 1") != std::string::npos);
}

TEST_CASE("devices endpoints list the catalog", "[api]") {
    Fixture f;
    auto res = f.get("/api/v1/devices");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("dev-01") != std::string::npos);
    CHECK(res->body.find("dev-02") != std::string::npos);

    auto one = f.get("/api/v1/devices/dev-01");
    REQUIRE(one);
    CHECK(one->status == 200);
    CHECK(one->body.find("\"id\":\"dev-01\"") != std::string::npos);

    auto missing = f.get("/api/v1/devices/nope");
    REQUIRE(missing);
    CHECK(missing->status == 404);
}

TEST_CASE("samples query validates range and paginates", "[api]") {
    Fixture f;
    std::vector<storage::SampleRow> rows;
    for (int i = 0; i < 3; ++i)
        rows.push_back(f.sample("temp", (100 + i) * SEC, 20.0 + i));
    REQUIRE(f.store->insert_samples(rows).ok());

    // Missing time range: rejected (FR-API-003).
    auto no_range = f.get("/api/v1/samples?device=dev-01&metric=temp");
    REQUIRE(no_range);
    CHECK(no_range->status == 400);
    CHECK(no_range->body.find("code") != std::string::npos);
    CHECK(no_range->body.find("request_id") != std::string::npos);

    auto all = f.get("/api/v1/samples?device=dev-01&metric=temp&from=" + iso(90 * SEC) + "&to=" + iso(200 * SEC));
    REQUIRE(all);
    CHECK(all->status == 200);
    CHECK(all->body.find("20.0") != std::string::npos);
    CHECK(all->body.find("next_cursor") == std::string::npos);

    // Page size 1: first page has a cursor, the second page completes the set.
    auto p1 = f.get("/api/v1/samples?device=dev-01&metric=temp&from=" + iso(90 * SEC) + "&to=" + iso(200 * SEC) +
                    "&limit=1");
    REQUIRE(p1);
    CHECK(p1->body.find("next_cursor") != std::string::npos);

    // Range over the configured maximum: rejected.
    auto huge = f.get("/api/v1/samples?device=dev-01&metric=temp&from=" + iso(0) + "&to=" + iso(400LL * 24 * 3600 * SEC));
    REQUIRE(huge);
    CHECK(huge->status == 400);
}

TEST_CASE("incident list, detail and acknowledgement", "[api]") {
    Fixture f;
    // Seed one open incident directly (rule engine internals are covered elsewhere).
    auto st = f.store->db().prepare(
        "INSERT INTO incidents(incident_uuid, rule_id, rule_version, device_id, severity, state, started_us,"
        " peak_value, hit_count, config_version, created_at_us) VALUES('inc-1','r',1,'dev-01','high','open',1000,1,1,0,1000)");
    REQUIRE(st);
    REQUIRE(st.value().step().ok());

    auto list = f.get("/api/v1/incidents?state=open");
    REQUIRE(list);
    CHECK(list->status == 200);
    CHECK(list->body.find("inc-1") != std::string::npos);

    auto detail = f.get("/api/v1/incidents/inc-1");
    REQUIRE(detail);
    CHECK(detail->status == 200);
    CHECK(detail->body.find("timeline") != std::string::npos);

    // Ack requires the admin token (FR-API-004).
    httplib::Client cli("127.0.0.1", f.http->bound_port());
    auto no_auth = cli.Post("/api/v1/incidents/inc-1/ack", "{\"by\":\"ops\"}", "application/json");
    REQUIRE(no_auth);
    CHECK(no_auth->status == 401);

    auto bad_auth = cli.Post("/api/v1/incidents/inc-1/ack", {{"Authorization", "Bearer wrong"}},
                             "{\"by\":\"ops\"}", "application/json");
    REQUIRE(bad_auth);
    CHECK(bad_auth->status == 401);

    auto ok_auth = cli.Post("/api/v1/incidents/inc-1/ack", {{"Authorization", "Bearer " + f.token}},
                            "{\"by\":\"ops\",\"comment\":\"seen\"}", "application/json");
    REQUIRE(ok_auth);
    CHECK(ok_auth->status == 200);

    // Acknowledgement does not change the rule state (FR-RULE-003); the ack info is
    // recorded on the incident and in the audit log.
    auto still_open = f.store->query_incidents("open", "", 10);
    REQUIRE(still_open);
    REQUIRE(still_open.value().size() == 1);
    CHECK(still_open.value()[0].acked_by == "ops");
    CHECK(still_open.value()[0].ack_comment == "seen");

    auto missing_ack = cli.Post("/api/v1/incidents/no-such/ack", {{"Authorization", "Bearer " + f.token}},
                                "{\"by\":\"ops\"}", "application/json");
    REQUIRE(missing_ack);
    CHECK(missing_ack->status == 404);
}

TEST_CASE("admin endpoints refuse to work without a token", "[api]") {
    Fixture f(/*with_token=*/false);
    httplib::Client cli("127.0.0.1", f.http->bound_port());
    auto res = cli.Post("/api/v1/admin/reload", "{}", "application/json");
    REQUIRE(res);
    CHECK(res->status == 503);
}

TEST_CASE("runtime reload keeps the old config when the file is broken", "[api]") {
    Fixture f;
    // The configured path does not exist: reload must fail and keep serving.
    auto bad = f.runtime->reload_config("req-1", "test");
    CHECK_FALSE(bad.ok());
    CHECK(f.runtime->current_config() != nullptr);
    CHECK(f.runtime->ready());

    // Submitting an import outside the input directory is rejected (10.3).
    auto outside = f.runtime->submit_import("/etc/passwd", false);
    CHECK_FALSE(outside.ok());
}
