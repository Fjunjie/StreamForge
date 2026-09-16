#include "streamforge/processing/calibration.hpp"

#include <cmath>

namespace streamforge {
namespace processing {

Result<CalibrationOutcome> apply_calibration(const ConfigSnapshot& cs, const std::string& device_id,
                                             const std::string& metric_id, double raw, double* out) {
    for (const auto& dc : cs.cfg.calibrations) {
        if (dc.device_id != device_id || dc.metric_id != metric_id)
            continue;
        for (const auto& seg : dc.calibration.segments) {
            if (raw >= seg.min_inclusive && raw < seg.max_exclusive) {
                double calibrated = seg.slope * raw + seg.intercept;
                if (!std::isfinite(calibrated)) {
                    return Result<CalibrationOutcome>::Err(
                        Error::make(ErrorCode::ValidationValueInvalid, "calibration produced a non-finite value"));
                }
                *out = calibrated;
                return Result<CalibrationOutcome>::Ok(CalibrationOutcome::Applied);
            }
        }
        if (dc.calibration.reject_unmatched) {
            return Result<CalibrationOutcome>::Err(
                Error::make(ErrorCode::ValidationValueInvalid, "no calibration segment matches the raw value")
                    .ctx("device", device_id)
                    .ctx("metric", metric_id));
        }
        *out = raw;
        return Result<CalibrationOutcome>::Ok(CalibrationOutcome::Passthrough);
    }
    *out = raw; // no calibration configured for this device+metric
    return Result<CalibrationOutcome>::Ok(CalibrationOutcome::Passthrough);
}

} // namespace processing
} // namespace streamforge
