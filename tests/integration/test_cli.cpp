#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

// End-to-end CLI integration: runs the real streamforgectl binary against a temporary
// workspace and verifies exit codes and database effects.
using namespace streamforge;
namespace fs = std::filesystem;

namespace {
std::string yaml_for(const sf_test::TempDir& dir) {
    return "schema_version: 1\n"
           "directories:\n"
           "  input: " +
           dir.path +
           "/input\n"
           "  archive: " +
           dir.path +
           "/archive\n"
           "  quarantine: " +
           dir.path +
           "/quarantine\n"
           "  reports: " +
           dir.path +
           "/reports\n"
           "database:\n"
           "  path: " +
           dir.path +
           "/cli.db\n"
           "pipeline:\n"
           "  batch_size: 2\n"
           "  max_error_rate: 0.10\n"
           "  error_rate_min_records: 20\n"
           "  history_range: 365d\n"
           "metrics:\n"
           "  - id: temp\n    canonical_unit: C\n    input_units: [C, F, K]\n"
           "devices:\n"
           "  - id: dev-01\n    timezone: UTC\n"
           "  - id: dev-02\n    timezone: Asia/Shanghai\n";
}

struct CliResult {
    int code = -1;
    std::string stdout_text;
};

CliResult run_ctl(const std::string& args) {
    CliResult r;
    std::string cmd = std::string(STREAMFORGE_CTL_BIN) + " " + args + " 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0)
        r.stdout_text.append(buf, n);
    int status = ::pclose(pipe);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

const char* kHeader = "device_id,metric,event_time,value,unit,quality,sequence,tags\n";
} // namespace

TEST_CASE("cli validate-config succeeds and fails correctly", "[cli][integration]") {
    sf_test::TempDir dir;
    auto good = dir.file("cfg.yaml", yaml_for(dir));
    CHECK(run_ctl("validate-config --config " + good).code == 0);

    auto bad = dir.file("bad.yaml", "schema_version: 99\n");
    CHECK(run_ctl("validate-config --config " + bad).code == 1);

    // Missing required argument -> usage error with exit code 2.
    CHECK(run_ctl("validate-config").code == 2);
    CHECK(run_ctl("no-such-command").code == 2);
}

TEST_CASE("cli import writes samples and query reads them back", "[cli][integration]") {
    sf_test::TempDir dir;
    auto cfg_path = dir.file("cfg.yaml", yaml_for(dir));
    std::string input = dir.sub("input");
    REQUIRE(::system(("mkdir -p " + input).c_str()) == 0);

    std::string rows;
    for (int i = 0; i < 5; ++i) {
        rows += std::string("dev-01,temp,2026-08-01T09:15:30.000Z,") + std::to_string(i) + ",C,0," + std::to_string(i) +
                ",\n";
    }
    auto csv = input + "/a.csv";
    {
        std::ofstream out(csv, std::ios::binary);
        out << kHeader << rows;
    }
    // Pin the mtime so that re-creating the file byte-for-byte reproduces the same
    // file identity (path + size + mtime + content hash, requirement FR-IN-003).
    REQUIRE(::system(("touch -d @1785575730 " + csv).c_str()) == 0);

    auto imported = run_ctl("import " + csv + " --config " + cfg_path);
    CHECK(imported.code == 0);
    CHECK(imported.stdout_text.find("success=1") != std::string::npos);

    // Re-import: the first run archived the file away; recreating it with the same bytes and
    // the same mtime reproduces the identity, so the run is skipped (still exit 0).
    {
        std::ofstream out(csv, std::ios::binary);
        out << kHeader << rows;
    }
    REQUIRE(::system(("touch -d @1785575730 " + csv).c_str()) == 0);
    auto again = run_ctl("import " + csv + " --config " + cfg_path);
    CHECK(again.code == 0);
    CHECK(again.stdout_text.find("skipped=1") != std::string::npos);

    // Directory import in lexicographic order; b.csv carries distinct sequences so its
    // samples are new rather than deduplicated against a.csv.
    auto b = input + "/b.csv";
    {
        std::string b_rows;
        for (int i = 0; i < 5; ++i) {
            b_rows += std::string("dev-01,temp,2026-08-01T09:15:30.000Z,") + std::to_string(i) + ",C,0," +
                      std::to_string(100 + i) + ",\n";
        }
        std::ofstream out(b, std::ios::binary);
        out << kHeader << b_rows;
    }
    auto dir_import = run_ctl("import " + input + " --config " + cfg_path);
    CHECK(dir_import.code == 0);

    auto status = run_ctl("status --config " + cfg_path + " --output json");
    CHECK(status.code == 0);
    CHECK(status.stdout_text.find("\"samples\":10") != std::string::npos);

    auto query = run_ctl("samples query --config " + cfg_path +
                         " --device dev-01 --metric temp --from 2026-08-01T00:00:00Z "
                         "--to 2026-08-02T00:00:00Z --output json");
    CHECK(query.code == 0);
    CHECK(query.stdout_text.find("dev-01") != std::string::npos);

    auto devices = run_ctl("devices list --config " + cfg_path + " --output json");
    CHECK(devices.code == 0);
    CHECK(devices.stdout_text.find("dev-02") != std::string::npos);

    auto migrate = run_ctl("db migrate --config " + cfg_path + " --output json");
    CHECK(migrate.code == 0);
    CHECK(migrate.stdout_text.find("schema_version") != std::string::npos);

    auto check = run_ctl("db check --config " + cfg_path + " --output json");
    CHECK(check.code == 0);
    CHECK(check.stdout_text.find("\"ok\":true") != std::string::npos);
}

TEST_CASE("cli import reports failure when a file is quarantined", "[cli][integration]") {
    sf_test::TempDir dir;
    auto cfg_path = dir.file("cfg.yaml", yaml_for(dir));
    std::string input = dir.sub("input");
    REQUIRE(::system(("mkdir -p " + input).c_str()) == 0);

    // 12 records with an unknown device => 12/20 = 60% business error rate >> threshold.
    std::string rows;
    for (int i = 0; i < 12; ++i) {
        rows += "ghost,temp,2026-08-01T09:15:30.000Z,1,C,0," + std::to_string(i) + ",\n";
    }
    auto bad = input + "/bad.csv";
    {
        std::ofstream out(bad, std::ios::binary);
        out << kHeader << rows;
    }

    auto result = run_ctl("import " + bad + " --config " + cfg_path);
    CHECK(result.code == 1);
    auto report_exists = [&] {
        for (const auto& entry : fs::directory_iterator(dir.path + "/quarantine")) {
            if (entry.path().filename() == "bad.csv.error.json")
                return true;
        }
        return false;
    }();
    CHECK(report_exists);
}
