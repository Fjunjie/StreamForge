#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "streamforge/core/time.hpp"
#include "streamforge/ingest/validation.hpp"
#include "support/test_env.hpp"

using namespace streamforge;

namespace {
RawRecord make_record() {
    RawRecord rec;
    rec.device_id = "dev-01";
    rec.metric = "temp";
    rec.event_time_raw = "2026-08-01T09:15:30Z";
    rec.has_value = true;
    rec.value = 21.5;
    rec.unit = "C";
    rec.line_no = 1;
    return rec;
}
} // namespace

TEST_CASE("valid record accepted", "[validation]") {
    auto cfg = sf_test::make_config(sf_test::TempDir().path);
    auto out = validate_record(make_record(), *cfg, parse_event_time("2026-08-01T09:16:00Z", nullptr, true).value().tp);
    CHECK(out.kind == ValidationKind::Accepted);
    CHECK(to_unix_us(out.event_time) == 1785575730000000LL); // 2026-08-01T09:15:30Z
}

TEST_CASE("check 1: charset and lengths", "[validation]") {
    auto cfg = sf_test::make_config(sf_test::TempDir().path);
    TimePointUs now{std::chrono::microseconds{1785575760000000LL}}; // 09:16:00Z

    auto rec = make_record();
    rec.device_id = "bad device!";
    auto out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::FormatError);
    CHECK(out.error.code == ErrorCode::ValidationCharset);

    rec = make_record();
    rec.metric = std::string(65, 'a');
    out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::FormatError);

    rec = make_record();
    rec.unit = "";
    out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::FormatError);
}

TEST_CASE("check 2: unknown device and metric are business errors", "[validation]") {
    auto cfg = sf_test::make_config(sf_test::TempDir().path);
    TimePointUs now{std::chrono::microseconds{1785575760000000LL}};

    auto rec = make_record();
    rec.device_id = "ghost";
    auto out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::BusinessError);
    CHECK(out.error.code == ErrorCode::ValidationDeviceUnknown);

    rec = make_record();
    rec.metric = "ghost_metric";
    out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::BusinessError);
    CHECK(out.error.code == ErrorCode::ValidationMetricUnknown);
}

TEST_CASE("check 3: time window rules", "[validation]") {
    auto cfg = sf_test::make_config(sf_test::TempDir().path);
    // now = 2026-08-01T09:16:00Z
    TimePointUs now{std::chrono::microseconds{1785575760000000LL}};

    // Too far in the future (> 10 minutes).
    auto rec = make_record();
    rec.event_time_raw = "2026-08-01T09:30:00Z";
    auto out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::BusinessError);
    CHECK(out.error.code == ErrorCode::ValidationTimeInvalid);

    // Unparseable syntax is a format error.
    rec.event_time_raw = "garbage";
    out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::FormatError);

    // Offset-less time resolved via the device timezone (dev-02 = Asia/Shanghai).
    rec = make_record();
    rec.device_id = "dev-02";
    rec.event_time_raw = "2026-08-01T17:16:00"; // == 09:16:00Z
    out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::Accepted);
}

TEST_CASE("check 4: non-finite value rejected as business error", "[validation]") {
    auto cfg = sf_test::make_config(sf_test::TempDir().path);
    TimePointUs now{std::chrono::microseconds{1785575760000000LL}};
    auto rec = make_record();
    rec.value = std::numeric_limits<double>::infinity();
    auto out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::BusinessError);
    CHECK(out.error.code == ErrorCode::ValidationValueInvalid);

    rec.value_is_null = true;
    rec.value = 0;
    out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::Accepted);
}

TEST_CASE("check 5: unit must be in the convertible set", "[validation]") {
    auto cfg = sf_test::make_config(sf_test::TempDir().path);
    TimePointUs now{std::chrono::microseconds{1785575760000000LL}};
    auto rec = make_record();
    rec.unit = "K";
    CHECK(validate_record(rec, *cfg, now).kind == ValidationKind::Accepted);
    rec.unit = "km/h";
    auto out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::BusinessError);
    CHECK(out.error.code == ErrorCode::ValidationUnitUnknown);
}

TEST_CASE("check 6: quality enum", "[validation]") {
    auto cfg = sf_test::make_config(sf_test::TempDir().path);
    TimePointUs now{std::chrono::microseconds{1785575760000000LL}};
    auto rec = make_record();
    rec.quality = 2;
    rec.has_quality = true;
    CHECK(validate_record(rec, *cfg, now).kind == ValidationKind::Accepted);
    rec.quality = 3;
    auto out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::BusinessError);
    CHECK(out.error.code == ErrorCode::ValidationQualityInvalid);
}

TEST_CASE("check 7: tag limits", "[validation]") {
    auto cfg = sf_test::make_config(sf_test::TempDir().path);
    TimePointUs now{std::chrono::microseconds{1785575760000000LL}};
    auto rec = make_record();
    rec.tags = {{"bad key!", "v"}};
    auto out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::BusinessError);
    CHECK(out.error.code == ErrorCode::ValidationTagsInvalid);

    rec.tags.clear();
    for (int i = 0; i < 17; ++i) {
        rec.tags.emplace_back("k" + std::to_string(i), "v");
    }
    out = validate_record(rec, *cfg, now);
    CHECK(out.kind == ValidationKind::BusinessError);
    CHECK(out.error.code == ErrorCode::ValidationTagsInvalid);
}
