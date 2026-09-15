#include "streamforge/ingest/validation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace streamforge {

namespace {

// device_id / metric: 1..64 chars, [A-Za-z0-9._-] (requirement FR-FMT-001).
bool valid_identifier(const std::string& id) {
    if (id.empty() || id.size() > 64)
        return false;
    for (char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

// unit: 1..32 printable ASCII characters, no whitespace.
bool valid_unit(const std::string& unit) {
    if (unit.empty() || unit.size() > 32)
        return false;
    for (char c : unit) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc >= 0x7F)
            return false;
    }
    return true;
}

// Tag keys: 1..128 chars of [A-Za-z0-9._-]; values: <= 128 bytes of UTF-8 without controls.
bool valid_tag_key(const std::string& key) {
    if (key.empty() || key.size() > 128)
        return false;
    for (char c : key) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

bool valid_tag_value(const std::string& value) {
    if (value.size() > 128)
        return false;
    for (char c : value) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F)
            return false;
    }
    return true;
}

} // namespace

ValidationOutcome validate_record(const RawRecord& rec, const ConfigSnapshot& cs, TimePointUs ingest_time) {
    ValidationOutcome out;

    // 1. Field length and character set (structural).
    if (!valid_identifier(rec.device_id)) {
        out.kind = ValidationKind::FormatError;
        out.error = Error::make(ErrorCode::ValidationCharset, "device_id must be 1..64 characters of [A-Za-z0-9._-]")
                        .ctx("device_id", rec.device_id);
        return out;
    }
    if (!valid_identifier(rec.metric)) {
        out.kind = ValidationKind::FormatError;
        out.error = Error::make(ErrorCode::ValidationCharset, "metric must be 1..64 characters of [A-Za-z0-9._-]")
                        .ctx("metric", rec.metric);
        return out;
    }
    if (!valid_unit(rec.unit)) {
        out.kind = ValidationKind::FormatError;
        out.error = Error::make(ErrorCode::ValidationCharset, "unit must be 1..32 printable ASCII characters")
                        .ctx("unit", rec.unit);
        return out;
    }

    // 2. Device and metric existence (business).
    const DeviceCfg* dev = cs.device(rec.device_id);
    if (dev == nullptr) {
        out.kind = ValidationKind::BusinessError;
        out.error = Error::make(ErrorCode::ValidationDeviceUnknown, "unknown device '" + rec.device_id + "'");
        return out;
    }
    const MetricCfg* metric = cs.metric(rec.metric);
    if (metric == nullptr) {
        out.kind = ValidationKind::BusinessError;
        out.error = Error::make(ErrorCode::ValidationMetricUnknown, "unknown metric '" + rec.metric + "'");
        return out;
    }
    if (!cs.metric_allowed_for_device(*dev, rec.metric)) {
        out.kind = ValidationKind::BusinessError;
        out.error = Error::make(ErrorCode::ValidationMetricUnknown,
                                "metric '" + rec.metric + "' is not allowed for device '" + rec.device_id + "'");
        return out;
    }

    // 3. Time parseable and inside the allowed window.
    const date::time_zone* tz = cs.device_tz(rec.device_id);
    bool ambiguous_earlier = cs.cfg.pipeline.ambiguous_time_policy == AmbiguousTimePolicy::Earlier;
    auto parsed = parse_event_time(rec.event_time_raw, tz, ambiguous_earlier);
    if (!parsed.ok()) {
        // Unparseable syntax is structural; DST/timezone semantics are business errors.
        if (parsed.error().code == ErrorCode::ValidationTimeInvalid &&
            (parsed.error().message.find("format") != std::string::npos ||
             parsed.error().message.find("offset requires") != std::string::npos)) {
            out.kind = ValidationKind::FormatError;
        } else {
            out.kind = ValidationKind::BusinessError;
        }
        out.error = parsed.error();
        out.error.ctx("line", std::to_string(rec.line_no));
        return out;
    }
    out.event_time = parsed.value().tp;
    out.leap_second = parsed.value().leap_second;

    const int64_t event_us = to_unix_us(out.event_time);
    const int64_t ingest_us = to_unix_us(ingest_time);
    const int64_t max_future_skew_us = 10LL * 60 * 1000000; // requirement FR-VAL-002
    if (event_us > ingest_us + max_future_skew_us) {
        out.kind = ValidationKind::BusinessError;
        out.error = Error::make(ErrorCode::ValidationTimeInvalid, "event time is more than 10 minutes in the future")
                        .ctx("line", std::to_string(rec.line_no));
        return out;
    }
    if (event_us < ingest_us - cs.cfg.pipeline.history_range_us) {
        out.kind = ValidationKind::BusinessError;
        out.error =
            Error::make(ErrorCode::ValidationTimeInvalid, "event time is older than the configured history range")
                .ctx("line", std::to_string(rec.line_no));
        return out;
    }

    // 4. Value finite or allowed null (business).
    if (!rec.has_value) {
        out.kind = ValidationKind::FormatError;
        out.error = Error::make(ErrorCode::FormatBadRecord, "missing required field 'value'")
                        .ctx("line", std::to_string(rec.line_no));
        return out;
    }
    if (!rec.value_is_null && !std::isfinite(rec.value)) {
        out.kind = ValidationKind::BusinessError;
        out.error = Error::make(ErrorCode::ValidationValueInvalid, "value must be finite")
                        .ctx("line", std::to_string(rec.line_no));
        return out;
    }

    // 5. Input unit in the metric's convertible set (business).
    if (std::find(metric->input_units.begin(), metric->input_units.end(), rec.unit) == metric->input_units.end()) {
        out.kind = ValidationKind::BusinessError;
        out.error = Error::make(ErrorCode::ValidationUnitUnknown,
                                "unit '" + rec.unit + "' is not convertible for metric '" + metric->id + "'")
                        .ctx("line", std::to_string(rec.line_no));
        return out;
    }

    // 6. Quality enum (business).
    if (rec.has_quality && (rec.quality < 0 || rec.quality > 2)) {
        out.kind = ValidationKind::BusinessError;
        out.error = Error::make(ErrorCode::ValidationQualityInvalid, "quality must be 0, 1 or 2")
                        .ctx("line", std::to_string(rec.line_no));
        return out;
    }

    // 7. Tag count, names and lengths (business).
    if (rec.tags.size() > 16) {
        out.kind = ValidationKind::BusinessError;
        out.error = Error::make(ErrorCode::ValidationTagsInvalid, "at most 16 tags per record")
                        .ctx("line", std::to_string(rec.line_no));
        return out;
    }
    for (const auto& t : rec.tags) {
        if (!valid_tag_key(t.first)) {
            out.kind = ValidationKind::BusinessError;
            out.error = Error::make(ErrorCode::ValidationTagsInvalid, "invalid tag key '" + t.first + "'")
                            .ctx("line", std::to_string(rec.line_no));
            return out;
        }
        if (!valid_tag_value(t.second)) {
            out.kind = ValidationKind::BusinessError;
            out.error = Error::make(ErrorCode::ValidationTagsInvalid, "invalid tag value for key '" + t.first + "'")
                            .ctx("line", std::to_string(rec.line_no));
            return out;
        }
    }

    out.kind = ValidationKind::Accepted;
    return out;
}

} // namespace streamforge
