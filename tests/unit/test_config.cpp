#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <memory>
#include <vector>

#include "streamforge/config/config.hpp"
#include "support/test_env.hpp"

using namespace streamforge;

namespace {
std::string yaml_config_path(const std::string& content) {
    // Keep temporary directories alive until process exit (tests are short-lived).
    static std::vector<std::unique_ptr<sf_test::TempDir>> keep_alive;
    keep_alive.push_back(std::make_unique<sf_test::TempDir>());
    auto& d = *keep_alive.back();
    return d.file("config.yaml", content);
}
} // namespace

TEST_CASE("example configuration loads", "[config]") {
    auto loaded = load_config(std::string(STREAMFORGE_SOURCE_DIR) + "/config/example.yaml");
    REQUIRE(loaded.ok());
    auto cs = loaded.take();
    CHECK(cs->cfg.schema_version == 1);
    CHECK(cs->version != 0);
    CHECK(cs->cfg.metrics.size() >= 3);
    CHECK(cs->cfg.devices.size() >= 2);
    CHECK_FALSE(cs->cfg.windows.empty());
}

TEST_CASE("missing schema_version rejected", "[config]") {
    auto path = yaml_config_path("server:\n  port: 8080\n");
    auto loaded = load_config(path);
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error().code == ErrorCode::ConfigValidation);
}

TEST_CASE("unknown schema major version rejected", "[config]") {
    auto path = yaml_config_path("schema_version: 2\n"
                                 "directories:\n"
                                 "  input: /tmp/i\n  archive: /tmp/a\n  quarantine: /tmp/q\n  reports: /tmp/r\n"
                                 "database:\n  path: /tmp/db.sqlite\n"
                                 "metrics:\n  - id: m\n    canonical_unit: C\n    input_units: [C]\n"
                                 "devices:\n  - id: d\n");
    auto loaded = load_config(path);
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error().code == ErrorCode::ConfigSchemaUnsupported);
}

TEST_CASE("duplicate ids rejected", "[config]") {
    auto path = yaml_config_path("schema_version: 1\n"
                                 "directories:\n"
                                 "  input: /tmp/i\n  archive: /tmp/a\n  quarantine: /tmp/q\n  reports: /tmp/r\n"
                                 "database:\n  path: /tmp/db.sqlite\n"
                                 "metrics:\n"
                                 "  - id: temp\n    canonical_unit: C\n    input_units: [C]\n"
                                 "  - id: temp\n    canonical_unit: K\n    input_units: [K]\n"
                                 "devices:\n  - id: d\n");
    auto loaded = load_config(path);
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error().code == ErrorCode::ConfigValidation);
}

TEST_CASE("unknown device timezone rejected", "[config]") {
    auto path = yaml_config_path("schema_version: 1\n"
                                 "directories:\n"
                                 "  input: /tmp/i\n  archive: /tmp/a\n  quarantine: /tmp/q\n  reports: /tmp/r\n"
                                 "database:\n  path: /tmp/db.sqlite\n"
                                 "metrics:\n  - id: temp\n    canonical_unit: C\n    input_units: [C]\n"
                                 "devices:\n  - id: d\n    timezone: Mars/Olympus\n");
    auto loaded = load_config(path);
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error().code == ErrorCode::ConfigValidation);
}

TEST_CASE("overlapping calibration segments rejected", "[config]") {
    auto path = yaml_config_path("schema_version: 1\n"
                                 "directories:\n"
                                 "  input: /tmp/i\n  archive: /tmp/a\n  quarantine: /tmp/q\n  reports: /tmp/r\n"
                                 "database:\n  path: /tmp/db.sqlite\n"
                                 "metrics:\n  - id: temp\n    canonical_unit: C\n    input_units: [C]\n"
                                 "devices:\n  - id: d\n"
                                 "calibrations:\n"
                                 "  - device: d\n    metric: temp\n"
                                 "    segments:\n"
                                 "      - {min: 0, max: 100, slope: 1, intercept: 0}\n"
                                 "      - {min: 50, max: 200, slope: 2, intercept: 1}\n");
    auto loaded = load_config(path);
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error().code == ErrorCode::ConfigValidation);
}

TEST_CASE("unknown optional fields produce warnings but load", "[config]") {
    auto path = yaml_config_path("schema_version: 1\n"
                                 "experimental_feature: true\n"
                                 "directories:\n"
                                 "  input: /tmp/i\n  archive: /tmp/a\n  quarantine: /tmp/q\n  reports: /tmp/r\n"
                                 "database:\n  path: /tmp/db.sqlite\n"
                                 "metrics:\n  - id: temp\n    canonical_unit: C\n    input_units: [C]\n"
                                 "devices:\n  - id: d\n");
    auto loaded = load_config(path);
    REQUIRE(loaded.ok());
}

TEST_CASE("invalid pipeline bounds rejected", "[config]") {
    auto path = yaml_config_path("schema_version: 1\n"
                                 "directories:\n"
                                 "  input: /tmp/i\n  archive: /tmp/a\n  quarantine: /tmp/q\n  reports: /tmp/r\n"
                                 "database:\n  path: /tmp/db.sqlite\n"
                                 "pipeline:\n  workers: 0\n"
                                 "metrics:\n  - id: temp\n    canonical_unit: C\n    input_units: [C]\n"
                                 "devices:\n  - id: d\n");
    auto loaded = load_config(path);
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error().code == ErrorCode::ConfigValidation);
}

TEST_CASE("relative paths resolve against the config file directory", "[config]") {
    sf_test::TempDir dir;
    auto path = dir.file("cfg.yaml", "schema_version: 1\n"
                                     "directories:\n"
                                     "  input: input\n  archive: archive\n  quarantine: quarantine\n"
                                     "  reports: reports\n"
                                     "database:\n  path: db/test.db\n"
                                     "metrics:\n  - id: temp\n    canonical_unit: C\n    input_units: [C]\n"
                                     "devices:\n  - id: d\n");
    auto loaded = load_config(path);
    REQUIRE(loaded.ok());
    CHECK(loaded.value()->cfg.directories.input == dir.path + "/input");
    CHECK(loaded.value()->cfg.database.path == dir.path + "/db/test.db");
}

TEST_CASE("parse_duration_us", "[config]") {
    CHECK(parse_duration_us("500ms").value() == 500000);
    CHECK(parse_duration_us("10s").value() == 10000000);
    CHECK(parse_duration_us("5m").value() == 300000000);
    CHECK(parse_duration_us("2h").value() == 7200000000LL);
    CHECK(parse_duration_us("7d").value() == 604800000000LL);
    CHECK_FALSE(parse_duration_us("10x").ok());
    CHECK_FALSE(parse_duration_us("").ok());
}
