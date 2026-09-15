#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

#include "streamforge/core/log.hpp"
#include "support/test_env.hpp"

using namespace streamforge;

namespace {
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("json log records are valid json objects", "[log]") {
    sf_test::TempDir dir;
    LogConfig cfg;
    cfg.level = "info";
    cfg.format = "json";
    cfg.file = dir.sub("logs") + "/test.log";
    cfg.quiet_console = true;
    init_logging(cfg);

    auto lg = logger("test_component");
    lg->warn("hello {} with \"quotes\" and\nnewline", 7);
    lg->flush();

    std::string content = slurp(cfg.file);
    REQUIRE_FALSE(content.empty());
    // Skip the possible file-creation/rotation header lines from spdlog (there are none),
    // take the last non-empty line.
    auto last = content.rfind('\n', content.size() - 2);
    std::string line = content.substr(last == std::string::npos ? 0 : last + 1);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();

    auto parsed = nlohmann::json::parse(line, nullptr, false);
    REQUIRE_FALSE(parsed.is_discarded());
    CHECK(parsed["level"] == "warning"); // spdlog spells warn-level as "warning"
    CHECK(parsed["component"] == "test_component");
    CHECK(parsed["msg"] == "hello 7 with \"quotes\" and\nnewline");
    CHECK(parsed.contains("ts"));
    CHECK(parsed.contains("thread"));
}

TEST_CASE("text log contains component and level", "[log]") {
    sf_test::TempDir dir;
    LogConfig cfg;
    cfg.level = "info";
    cfg.format = "text";
    cfg.file = dir.sub("logs") + "/test.log";
    cfg.quiet_console = true;
    init_logging(cfg);

    logger("worker")->error("boom happened");
    logger("worker")->flush();

    std::string content = slurp(cfg.file);
    REQUIRE(content.find("[worker]") != std::string::npos);
    REQUIRE(content.find("[error]") != std::string::npos);
    REQUIRE(content.find("boom happened") != std::string::npos);
}
