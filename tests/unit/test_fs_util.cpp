#include <catch2/catch_test_macros.hpp>

#include "streamforge/core/fs_util.hpp"

using namespace streamforge;

TEST_CASE("canonical_abs normalizes paths", "[fs]") {
    auto cwd = canonical_abs(".");
    REQUIRE(cwd.ok());

    auto nested = canonical_abs("a/b/../c");
    REQUIRE(nested.ok());
    CHECK(nested.value().find("..") == std::string::npos);
    CHECK(nested.value().compare(0, cwd.value().size(), cwd.value()) == 0);

    CHECK_FALSE(canonical_abs("").ok());
}

TEST_CASE("path_within enforces containment", "[fs]") {
    CHECK(path_within("/data", "/data/input/a.csv"));
    CHECK(path_within("/data", "/data"));
    CHECK_FALSE(path_within("/data", "/database/x"));
    CHECK_FALSE(path_within("/data", "/data/../etc/passwd"));
    CHECK(path_within("/data/", "/data/x"));
}

TEST_CASE("sanitize_raw strips control characters and truncates", "[fs]") {
    std::string raw = "a\x01"
                      "b\x7f"
                      "c\n";
    auto out = sanitize_raw(raw, 100);
    CHECK(out == "abc");

    std::string long_input(1000, 'x');
    CHECK(sanitize_raw(long_input, 256).size() == 256);
}
