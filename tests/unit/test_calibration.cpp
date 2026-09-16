#include <catch2/catch_test_macros.hpp>

#include "streamforge/processing/calibration.hpp"
#include "support/test_env.hpp"

using namespace streamforge;
using streamforge::processing::apply_calibration;
using streamforge::processing::CalibrationOutcome;

namespace {
// Config with one calibration: segments [-50,0) slope 2 intercept 1, [0,100) slope 0.5
// intercept 10, reject_unmatched configurable.
std::shared_ptr<const ConfigSnapshot> calib_config(const std::string& root, bool reject_unmatched) {
    auto cfg = std::const_pointer_cast<ConfigSnapshot>(sf_test::make_config(root));
    Config& c = cfg->cfg;
    DeviceCalibration dc;
    dc.device_id = "dev-01";
    dc.metric_id = "temp";
    dc.calibration.reject_unmatched = reject_unmatched;
    dc.calibration.segments = {
        CalibrationSegment{-50.0, 0.0, 2.0, 1.0},
        CalibrationSegment{0.0, 100.0, 0.5, 10.0},
    };
    c.calibrations.push_back(dc);
    cfg->version = compute_config_version(c);
    return cfg;
}
} // namespace

TEST_CASE("calibration matches closed-open segments", "[calibration]") {
    sf_test::TempDir dir;
    auto cfg = calib_config(dir.path, false);

    double out = 0;
    // [0,100): slope 0.5, intercept 10.
    auto r = apply_calibration(*cfg, "dev-01", "temp", 10.0, &out);
    REQUIRE(r.ok());
    CHECK(r.value() == CalibrationOutcome::Applied);
    CHECK(out == 15.0);
    // Boundary: min is inclusive, max exclusive.
    r = apply_calibration(*cfg, "dev-01", "temp", 0.0, &out);
    REQUIRE(r.ok());
    CHECK(out == 10.0); // second segment
    r = apply_calibration(*cfg, "dev-01", "temp", -50.0, &out);
    REQUIRE(r.ok());
    CHECK(out == -99.0); // first segment: 2 * -50 + 1
    r = apply_calibration(*cfg, "dev-01", "temp", 100.0, &out);
    REQUIRE(r.ok());
    CHECK(r.value() == CalibrationOutcome::Passthrough); // above all segments
    CHECK(out == 100.0);
}

TEST_CASE("calibration passthrough vs reject for unmatched raw values", "[calibration]") {
    sf_test::TempDir dir;
    double out = 0;

    auto passthrough = calib_config(dir.path, false);
    auto r = apply_calibration(*passthrough, "dev-01", "temp", 500.0, &out);
    REQUIRE(r.ok());
    CHECK(r.value() == CalibrationOutcome::Passthrough);
    CHECK(out == 500.0);

    auto reject = calib_config(dir.path, true);
    r = apply_calibration(*reject, "dev-01", "temp", 500.0, &out);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::ValidationValueInvalid);
}

TEST_CASE("calibration applies only to the configured device and metric", "[calibration]") {
    sf_test::TempDir dir;
    auto cfg = calib_config(dir.path, false);
    double out = 0;
    // dev-02 has no calibration: raw passes through untouched.
    auto r = apply_calibration(*cfg, "dev-02", "temp", 7.0, &out);
    REQUIRE(r.ok());
    CHECK(r.value() == CalibrationOutcome::Passthrough);
    CHECK(out == 7.0);
}
