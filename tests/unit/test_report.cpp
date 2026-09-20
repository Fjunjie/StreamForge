#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "streamforge/core/uuid.hpp"
#include "streamforge/report/report.hpp"
#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using namespace streamforge::report;
namespace fs = std::filesystem;

namespace {
constexpr int64_t SEC = 1000000LL;

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
} // namespace

TEST_CASE("report type and format parsing", "[report]") {
    CHECK(parse_type("summary").ok());
    CHECK(parse_type("incident").ok());
    CHECK(parse_type("quality").ok());
    CHECK_FALSE(parse_type("other").ok());
    CHECK(parse_format("csv").ok());
    CHECK(parse_format("json").ok());
    CHECK_FALSE(parse_format("xlsx").ok());
}

TEST_CASE("summary report writes csv and json atomically", "[report]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto opened = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(opened.ok());
    auto store = opened.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    std::vector<storage::SampleRow> rows;
    for (int i = 0; i < 4; ++i)
        rows.push_back(sample("temp", (10 + i) * SEC, 10.0 + i));
    REQUIRE(store->insert_samples(rows).ok());

    Request req;
    req.type = Type::Summary;
    req.from_us = SEC;
    req.to_us = 100 * SEC;
    req.output_path = dir.path + "/summary.csv";
    auto result = generate(*store, *cfg, req);
    REQUIRE(result.ok());
    CHECK(result.value().rows == 1); // one (device, metric) group
    CHECK(fs::exists(req.output_path));
    // No temp leftovers.
    for (const auto& e : fs::directory_iterator(dir.path))
        CHECK(e.path().filename().string().find(".tmp-") == std::string::npos);

    std::ifstream in(req.output_path);
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    // Meta columns, formula-safe cells, and the aggregated statistics are present.
    CHECK(text.find("report_type") != std::string::npos);
    CHECK(text.find("config_version") != std::string::npos);
    CHECK(text.find("dev-01") != std::string::npos);
    CHECK(text.find("sample_count") != std::string::npos);
    CHECK(text.find("avg") != std::string::npos);
    // CRLF row terminators (RFC 4180 style, consistent with the CSV parser).
    CHECK(text.find("\r\n") != std::string::npos);

    // JSON variant carries the meta block and machine-readable numbers.
    Request jreq = req;
    jreq.format = Format::Json;
    jreq.output_path = dir.path + "/summary.json";
    auto jresult = generate(*store, *cfg, jreq);
    REQUIRE(jresult.ok());
    std::ifstream jin(jreq.output_path);
    std::stringstream jbuf;
    jbuf << jin.rdbuf();
    const std::string jtext = jbuf.str();
    CHECK(jtext.find("\"meta\"") != std::string::npos);
    CHECK(jtext.find("\"avg\"") != std::string::npos);
}

TEST_CASE("incident and quality reports render", "[report]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto opened = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(opened.ok());
    auto store = opened.take();
    REQUIRE(store->sync_catalog(*cfg).ok());

    auto st = store->db().prepare(
        "INSERT INTO incidents(incident_uuid, rule_id, rule_version, device_id, severity, state, started_us,"
        " peak_value, hit_count, config_version, created_at_us) VALUES('inc-9','hot',1,'dev-01','high','open',5000000,1,1,0,5000000)");
    REQUIRE(st.ok());
    REQUIRE(st.value().step().ok());

    Request req;
    req.type = Type::Incident;
    req.from_us = SEC;
    req.to_us = 100 * SEC;
    req.output_path = dir.path + "/inc.csv";
    auto result = generate(*store, *cfg, req);
    REQUIRE(result.ok());
    CHECK(result.value().rows == 1);

    std::ifstream in(req.output_path);
    std::stringstream buf;
    buf << in.rdbuf();
    CHECK(buf.str().find("inc-9") != std::string::npos);

    Request qreq;
    qreq.type = Type::Quality;
    qreq.from_us = SEC;
    qreq.to_us = 100 * SEC;
    qreq.output_path = dir.path + "/quality.json";
    auto qresult = generate(*store, *cfg, qreq);
    REQUIRE(qresult.ok());
    CHECK(qresult.value().rows == 1);
}

TEST_CASE("report validates its request", "[report]") {
    sf_test::TempDir dir;
    auto cfg = sf_test::make_config(dir.path);
    auto opened = storage::Store::open(cfg->cfg.database.path);
    REQUIRE(opened.ok());
    auto store = opened.take();

    Request req;
    req.from_us = 100;
    req.to_us = 100; // from == to: rejected
    req.output_path = dir.path + "/x.csv";
    CHECK_FALSE(generate(*store, *cfg, req).ok());
    req.to_us = 200;
    req.output_path.clear(); // no output path: rejected
    CHECK_FALSE(generate(*store, *cfg, req).ok());
}
