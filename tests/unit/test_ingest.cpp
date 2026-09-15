#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <thread>

#include "streamforge/core/fs_util.hpp"
#include "streamforge/ingest/archive.hpp"
#include "streamforge/ingest/discovery.hpp"
#include "streamforge/ingest/error_report.hpp"
#include "streamforge/ingest/file_identity.hpp"
#include "streamforge/ingest/format.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
namespace fs = std::filesystem;

TEST_CASE("format detection and ready suffix", "[ingest]") {
    CHECK(format_from_extension("csv") == InputFormat::Csv);
    CHECK(format_from_extension("jsonl") == InputFormat::Jsonl);
    CHECK(format_from_extension("json") == InputFormat::Jsonl);
    CHECK(format_from_extension("tlm") == InputFormat::Tlm);
    CHECK(format_from_extension("txt") == InputFormat::Unknown);
    CHECK(strip_ready_suffix("a.csv.ready") == "a.csv");
    CHECK(strip_ready_suffix("a.csv") == "a.csv");
    CHECK(format_name(InputFormat::Csv) == std::string("csv"));
}

TEST_CASE("file identity is stable and content sensitive", "[ingest]") {
    sf_test::TempDir dir;
    auto path1 = dir.file("a.csv", "device_id,metric,event_time,value,unit\n");
    auto id1 = compute_file_identity(path1);
    REQUIRE(id1.ok());
    CHECK(id1.value().sha256_64k.size() == 64);
    CHECK(id1.value().identity_hash.size() == 64);
    CHECK(id1.value().size_bytes == static_cast<int64_t>(fs::file_size(fs::path(path1))));

    // Same content, same identity.
    auto id1b = compute_file_identity(path1);
    REQUIRE(id1b.ok());
    CHECK(id1b.value().identity_hash == id1.value().identity_hash);

    // Content change: new identity.
    auto path2 = dir.file("a.csv", "device_id,metric,event_time,value,unit,x\n");
    auto id2 = compute_file_identity(path2);
    REQUIRE(id2.ok());
    CHECK(id2.value().identity_hash != id1.value().identity_hash);

    // Missing file: error.
    CHECK_FALSE(compute_file_identity(dir.path + "/missing.csv").ok());

    // Symlinks are not followed.
    std::error_code ec;
    fs::create_symlink(path1, fs::path(dir.path) / "link.csv", ec);
    auto link_identity = compute_file_identity((fs::path(dir.path) / "link.csv").string());
    CHECK_FALSE(link_identity.ok());
}

TEST_CASE("directory scanner discovers ready files and quiet files", "[ingest]") {
    sf_test::TempDir dir;
    std::string input = dir.sub("input");
    dir.file("input/ready.csv.ready", "device_id,metric,event_time,value,unit\n");
    dir.file("input/quiet.csv", "device_id,metric,event_time,value,unit\n");
    dir.file("input/.hidden.csv", "");
    dir.file("input/noise.txt", "");
    dir.file("input/partial.csv.tmp", "");

    DirectoryScanner scanner(input, std::chrono::milliseconds(0));
    auto scan1 = scanner.scan();
    REQUIRE(scan1.ok());
    // Scan 1: ready file is a candidate; quiet.csv is new and needs a second scan.
    REQUIRE(scan1.value().size() == 1);
    CHECK(scan1.value()[0].path == input + "/ready.csv.ready");
    CHECK(scan1.value()[0].via_ready);

    auto scan2 = scanner.scan();
    REQUIRE(scan2.ok());
    REQUIRE(scan2.value().size() == 1);
    CHECK(scan2.value()[0].path == input + "/quiet.csv");
    CHECK_FALSE(scan2.value()[0].via_ready);

    // Third scan: no new candidates.
    auto scan3 = scanner.scan();
    REQUIRE(scan3.ok());
    CHECK(scan3.value().empty());
}

TEST_CASE("archive move creates date layout and never overwrites", "[ingest]") {
    sf_test::TempDir dir;
    std::string archive = dir.sub("archive");
    auto src = dir.file("a.csv", "payload");
    auto ident = compute_file_identity(src);
    REQUIRE(ident.ok());

    auto dest1 = archive_file(src, archive, ident.value().identity_hash);
    REQUIRE(dest1.ok());
    CHECK(fs::path(dest1.value()).filename().string() == "a.csv");
    CHECK_FALSE(fs::exists(src));

    // Same name again: second file gets the short hash appended.
    auto src2 = dir.file("a.csv", "payload");
    auto dest2 = archive_file(src2, archive, ident.value().identity_hash);
    REQUIRE(dest2.ok());
    CHECK(dest2.value() != dest1.value());
    CHECK(fs::path(dest2.value()).filename().string().find('-') != std::string::npos);
}

TEST_CASE("quarantine writes error.json next to the file", "[ingest]") {
    sf_test::TempDir dir;
    std::string quarantine = dir.sub("quarantine");
    auto src = dir.file("bad.csv", "garbage");
    auto ident = compute_file_identity(src);
    REQUIRE(ident.ok());

    auto report = build_error_report(src, ident.value().identity_hash, "TestError", "test", 10, 5, 1, 0.5, 1,
                                     {SampleError{1, "TestError", "m", "raw"}});
    auto dest = quarantine_file(src, quarantine, ident.value().identity_hash, report);
    REQUIRE(dest.ok());
    CHECK(fs::exists(dest.value() + ".error.json"));
    std::ifstream in(dest.value() + ".error.json");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content.find("TestError") != std::string::npos);
    CHECK_FALSE(fs::exists(src));
}
