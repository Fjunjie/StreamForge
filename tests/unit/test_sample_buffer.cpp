#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "streamforge/processing/sample_buffer.hpp"

using namespace streamforge;
using namespace streamforge::processing;

namespace {
constexpr int64_t SEC = 1000000LL;

void feed(SampleBuffer& buf, int64_t t_us, double v) {
    buf.add("dev-01", "temp", t_us, v);
}
} // namespace

TEST_CASE("sample buffer tracks latest values per device and metric", "[sample_buffer]") {
    SampleBuffer buf(60 * SEC);
    buf.add("dev-01", "temp", 1 * SEC, 10.0);
    buf.add("dev-01", "temp", 2 * SEC, 11.0);
    buf.add("dev-01", "pressure", 2 * SEC, 100.0);
    buf.add("dev-02", "temp", 3 * SEC, 50.0);

    auto lt = buf.latest("dev-01", "temp");
    REQUIRE(lt.has_value());
    CHECK(lt->value == 11.0);
    CHECK(lt->time_us == 2 * SEC);

    auto by_metric = buf.latest_by_metric("dev-01");
    REQUIRE(by_metric.size() == 2);
    CHECK(by_metric.at("temp").value == 11.0);
    CHECK(by_metric.at("pressure").value == 100.0);

    CHECK_FALSE(buf.latest("dev-03", "temp").has_value());
}

TEST_CASE("sample buffer window aggregates", "[sample_buffer]") {
    SampleBuffer buf(60 * SEC);
    // 5 samples, 10s apart: values 0..40.
    for (int i = 0; i < 5; ++i)
        feed(buf, i * 10 * SEC, i * 10.0);

    // avg over the trailing 60s: samples 0..40 (all five).
    auto avg = buf.window("dev-01", "temp", 60 * SEC, "avg");
    REQUIRE(avg.has_value());
    CHECK(*avg == Catch::Approx(20.0));

    // avg over the trailing 30s: window [10s, 40s] holds samples 10,20,30,40.
    auto avg30 = buf.window("dev-01", "temp", 30 * SEC, "avg");
    REQUIRE(avg30.has_value());
    CHECK(*avg30 == Catch::Approx(25.0));

    // delta over the trailing 30s: 40 - 10.
    auto delta = buf.window("dev-01", "temp", 30 * SEC, "delta");
    REQUIRE(delta.has_value());
    CHECK(*delta == Catch::Approx(30.0));

    // rate: 30 units over 30 seconds.
    auto rate = buf.window("dev-01", "temp", 30 * SEC, "rate");
    REQUIRE(rate.has_value());
    CHECK(*rate == Catch::Approx(1.0));

    // single sample in the window: avg works, delta/rate need two.
    auto avg10 = buf.window("dev-01", "temp", 1 * SEC, "avg");
    REQUIRE(avg10.has_value());
    CHECK(*avg10 == 40.0);
    CHECK_FALSE(buf.window("dev-01", "temp", 1 * SEC, "delta").has_value());

    // unknown function name.
    CHECK_FALSE(buf.window("dev-01", "temp", 30 * SEC, "median").has_value());
    CHECK_FALSE(buf.window("dev-01", "nope", 30 * SEC, "avg").has_value());
}

TEST_CASE("sample buffer evicts by age and count, ignoring late samples", "[sample_buffer]") {
    SampleBuffer buf(60 * SEC, /*max_samples=*/4);
    for (int i = 0; i < 10; ++i)
        feed(buf, i * SEC, i * 1.0);
    // Count cap of 4 dominates (samples span 9s < 60s).
    CHECK(buf.size("dev-01", "temp") == 4);

    // The window is anchored at the newest sample: age eviction keeps it at 4 entries.
    SampleBuffer by_age(30 * SEC);
    for (int i = 0; i < 10; ++i)
        by_age.add("d", "m", i * 10 * SEC, i * 1.0);
    // Newest at 90s; entries older than 90-30=60s were evicted on the way.
    CHECK(by_age.size("d", "m") == 4);
    auto w = by_age.window("d", "m", 30 * SEC, "avg");
    REQUIRE(w.has_value());
    CHECK(*w == Catch::Approx((6.0 + 7.0 + 8.0 + 9.0) / 4.0));

    // Late samples do not move the ring backwards.
    by_age.add("d", "m", 5 * SEC, 999.0);
    CHECK(by_age.size("d", "m") == 4);
    auto lt = by_age.latest("d", "m");
    REQUIRE(lt.has_value());
    CHECK(lt->time_us == 90 * SEC);
}
