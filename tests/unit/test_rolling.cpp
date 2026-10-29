#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "streamforge/processing/rolling.hpp"

using streamforge::processing::RollingSeries;

TEST_CASE("rolling mean and stddev are incrementally maintained", "[rolling]") {
    RollingSeries s(4, 0); // last 4 samples
    s.add(1000, 1.0);
    s.add(2000, 2.0);
    s.add(3000, 3.0);
    s.add(4000, 4.0);
    CHECK(s.size() == 4);
    CHECK(s.ready());
    CHECK(s.mean() == 2.5);
    CHECK(s.min() == 1.0);
    CHECK(s.max() == 4.0);
    // Population stddev of 1..4 = sqrt(1.25).
    CHECK(std::fabs(s.stddev() - 1.118033988749895) < 1e-12);

    // Adding a fifth evicts the oldest: window 2..5.
    s.add(5000, 5.0);
    CHECK(s.size() == 4);
    CHECK(s.mean() == 3.5);
    CHECK(s.min() == 2.0);
    CHECK(s.oldest() == 2.0);
    CHECK(s.newest() == 5.0);
    CHECK(s.rate_per_second() == Catch::Approx(1000.0)); // (5-2) over (5000us-2000us)=3ms
}

TEST_CASE("duration based pruning drops old samples", "[rolling]") {
    RollingSeries s(0 /* no count cap */, 5'000'000 /* 5s */);
    s.add(1'000'000, 1.0);
    s.add(4'000'000, 2.0);
    s.add(5'500'000, 3.0);
    CHECK(s.size() == 3);
    s.add(6'500'000, 4.0); // newest 6.5s: drops 1.0s (older than newest - 5s)
    CHECK(s.size() == 3);
    CHECK(s.oldest() == 2.0);
    CHECK(s.newest() == 4.0);
}

TEST_CASE("rate is zero without a positive time span", "[rolling]") {
    RollingSeries s(10, 0);
    s.add(1000, 5.0);
    CHECK(s.rate_per_second() == 0.0);
    s.add(1000, 7.0); // same timestamp
    CHECK(s.rate_per_second() == 0.0);
}
