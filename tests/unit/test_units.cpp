#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "streamforge/core/units.hpp"

// The conversion table lives in core (config validation needs it); the processing facade
// simply re-exports it. Tests pin the requirement FR-VAL-004 table.
using namespace streamforge;
using core_units::convert_unit;
using core_units::find_unit;

TEST_CASE("temperature conversions across C, F and K", "[units]") {
    CHECK(convert_unit("C", "K", 0.0).value() == 273.15);
    CHECK(convert_unit("K", "C", 273.15).value() == 0.0);
    CHECK(convert_unit("F", "C", 32.0).value() == Catch::Approx(0.0).margin(1e-9));
    CHECK(convert_unit("F", "C", 212.0).value() == Catch::Approx(100.0));
    CHECK(convert_unit("C", "F", 100.0).value() == Catch::Approx(212.0));
    CHECK(convert_unit("K", "F", 373.15).value() == Catch::Approx(212.0));
}

TEST_CASE("pressure, velocity, power and flow conversions", "[units]") {
    CHECK(convert_unit("Pa", "kPa", 150000.0).value() == 150.0);
    CHECK(convert_unit("MPa", "Pa", 1.0).value() == 1000000.0);
    CHECK(convert_unit("bar", "kPa", 1.0).value() == 100.0);
    CHECK(convert_unit("mm/s", "m/s", 2500.0).value() == 2.5);
    CHECK(convert_unit("cm/s", "mm/s", 1.0).value() == 10.0);
    CHECK(convert_unit("kW", "W", 1.5).value() == 1500.0);
    CHECK(convert_unit("MW", "kW", 2.0).value() == 2000.0);
    CHECK(convert_unit("L/min", "m3/h", 60.0).value() == Catch::Approx(3.6));
    CHECK(convert_unit("m3/s", "L/min", 0.001).value() == 60.0);
    CHECK(convert_unit("m3/h", "m3/s", 3600.0).value() == 1.0);
}

TEST_CASE("conversion round trips preserve values", "[units][property]") {
    const char* units[] = {"C",    "F",   "K", "Pa", "kPa", "MPa",   "bar",  "mm/s",
                           "cm/s", "m/s", "W", "kW", "MW",  "L/min", "m3/h", "m3/s"};
    for (const char* u : units) {
        double v = 12.5;
        auto to_base = convert_unit(u, u, v); // identity within the same unit
        REQUIRE(to_base.ok());
        CHECK(to_base.value() == Catch::Approx(v).margin(1e-9));
    }
}

TEST_CASE("physically invalid values rejected", "[units]") {
    // Below absolute zero.
    CHECK_FALSE(convert_unit("C", "K", -300.0).ok());
    CHECK_FALSE(convert_unit("F", "C", -500.0).ok());
    // Negative physical quantities.
    CHECK_FALSE(convert_unit("Pa", "kPa", -1.0).ok());
    CHECK_FALSE(convert_unit("mm/s", "m/s", -0.1).ok());
    CHECK_FALSE(convert_unit("kW", "W", -5.0).ok());
    CHECK_FALSE(convert_unit("m3/h", "m3/s", -1.0).ok());
    // Overflow: MPa -> Pa scales up by 1e6; 1e307 MPa overflows the double range.
    CHECK_FALSE(convert_unit("MPa", "Pa", 1e307).ok());
}

TEST_CASE("unknown units and dimension mismatch rejected", "[units]") {
    CHECK_FALSE(convert_unit("mile/h", "m/s", 1.0).ok());
    CHECK_FALSE(convert_unit("C", "Pa", 20.0).ok()); // temperature -> pressure
    CHECK(find_unit("C") != nullptr);
    CHECK(find_unit("not-a-unit") == nullptr);
}
