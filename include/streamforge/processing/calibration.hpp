#pragma once

#include <string>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"

namespace streamforge {
namespace processing {

// Piecewise linear calibration (requirement FR-VAL-003):
//   calibrated = slope * raw + intercept, segments matched on [min, max).
// A device+metric without any configured calibration passes the raw value through.
// When segments exist but none matches, behaviour follows the configured
// reject_unmatched flag: reject the sample (error) or pass the raw value through.
enum class CalibrationOutcome { Applied, Passthrough };

Result<CalibrationOutcome> apply_calibration(const ConfigSnapshot& cs, const std::string& device_id,
                                             const std::string& metric_id, double raw, double* out);

} // namespace processing
} // namespace streamforge
