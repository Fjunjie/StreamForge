#include <catch2/catch_test_macros.hpp>

#include "streamforge/core/time.hpp"

using namespace streamforge;

TEST_CASE("parse unix milliseconds", "[time]") {
    // 2026-08-01T15:35:30.125Z == 1785598530125 ms (verified against datetime).
    auto parsed = parse_event_time("1785598530125", nullptr, true);
    REQUIRE(parsed.ok());
    CHECK_FALSE(parsed.value().leap_second);
    CHECK(format_utc_us(parsed.value().tp) == "2026-08-01T15:35:30.125000Z");
}

TEST_CASE("parse ISO 8601 with offsets", "[time]") {
    auto parsed = parse_event_time("2026-08-01T09:15:30.125+08:00", nullptr, true);
    REQUIRE(parsed.ok());
    CHECK(format_utc_us(parsed.value().tp) == "2026-08-01T01:15:30.125000Z");

    auto z = parse_event_time("2026-08-01T01:15:30Z", nullptr, true);
    REQUIRE(z.ok());
    CHECK(format_utc_us(z.value().tp) == "2026-08-01T01:15:30.000000Z");

    auto compact = parse_event_time("2026-08-01T01:15:30.5+0000", nullptr, true);
    REQUIRE(compact.ok());
    CHECK(format_utc_us(compact.value().tp) == "2026-08-01T01:15:30.500000Z");
}

TEST_CASE("offset-less time uses the device timezone", "[time]") {
    auto* shanghai = locate_timezone("Asia/Shanghai");
    REQUIRE(shanghai != nullptr);
    auto parsed = parse_event_time("2026-08-01T09:15:30", shanghai, true);
    REQUIRE(parsed.ok());
    CHECK(format_utc_us(parsed.value().tp) == "2026-08-01T01:15:30.000000Z");

    // UTC device: no conversion.
    auto utc = parse_event_time("2026-08-01T09:15:30", locate_timezone("UTC"), true);
    REQUIRE(utc.ok());
    CHECK(format_utc_us(utc.value().tp) == "2026-08-01T09:15:30.000000Z");
}

TEST_CASE("DST gap rejected, ambiguous time resolved by policy", "[time][dst]") {
    auto* ny = locate_timezone("America/New_York");
    REQUIRE(ny != nullptr);

    // 2026-03-08 02:30 does not exist (spring forward in the US).
    auto gap = parse_event_time("2026-03-08T02:30:00", ny, true);
    REQUIRE_FALSE(gap.ok());
    CHECK(gap.error().code == ErrorCode::ValidationTimeInvalid);
    CHECK(gap.error().message.find("DST gap") != std::string::npos);

    // 2026-11-01 01:30 occurs twice: earlier = EDT (-04:00), later = EST (-05:00).
    auto earlier = parse_event_time("2026-11-01T01:30:00", ny, true);
    REQUIRE(earlier.ok());
    CHECK(format_utc_us(earlier.value().tp) == "2026-11-01T05:30:00.000000Z");
    auto later = parse_event_time("2026-11-01T01:30:00", ny, false);
    REQUIRE(later.ok());
    CHECK(format_utc_us(later.value().tp) == "2026-11-01T06:30:00.000000Z");
}

TEST_CASE("leap second :60 normalizes to the next minute", "[time][leap]") {
    auto parsed = parse_event_time("2026-06-30T23:59:60Z", nullptr, true);
    REQUIRE(parsed.ok());
    CHECK(parsed.value().leap_second);
    CHECK(format_utc_us(parsed.value().tp) == "2026-07-01T00:00:00.000000Z");

    auto frac = parse_event_time("2026-06-30T23:59:60.500Z", nullptr, true);
    REQUIRE(frac.ok());
    CHECK(frac.value().leap_second);
    CHECK(format_utc_us(frac.value().tp) == "2026-07-01T00:00:00.500000Z");
}

TEST_CASE("invalid times rejected", "[time]") {
    CHECK_FALSE(parse_event_time("not-a-time", nullptr, true).ok());
    CHECK_FALSE(parse_event_time("2026-13-01T00:00:00Z", nullptr, true).ok());
    CHECK_FALSE(parse_event_time("2026-02-30T00:00:00Z", nullptr, true).ok());
    CHECK_FALSE(parse_event_time("", nullptr, true).ok());
    CHECK_FALSE(parse_event_time("2026-08-01T09:15:30.125+99:00", nullptr, true).ok());
}

TEST_CASE("admin time parses offset-less values as UTC", "[time]") {
    auto t = parse_admin_time("2026-08-01T00:00:00");
    REQUIRE(t.ok());
    CHECK(format_utc_us(t.value()) == "2026-08-01T00:00:00.000000Z");
}

TEST_CASE("format keeps microseconds", "[time]") {
    TimePointUs tp{std::chrono::microseconds{1785598530123456}};
    CHECK(format_utc_us(tp) == "2026-08-01T15:35:30.123456Z");
}
