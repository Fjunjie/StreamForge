#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"
#include "streamforge/processing/types.hpp"
#include "streamforge/storage/store.hpp"

namespace streamforge {
namespace processing {

// Missing-data detection and interpolation (requirements FR-MIS-001/FR-MIS-002).
//
// The detector tracks the last VALID sample (quality != device error) per
// (device, metric). When a new valid sample arrives with gap = t - last_t > period +
// jitter, a missing interval [last_t, t] with the expected point count is produced
// (always — interpolation never removes the interval). When the configured interpolation
// policy is previous/linear and gap <= max_gap, synthetic samples (flagged kSynthetic)
// are produced for the interior points of the gap. Device-error-quality samples never
// become interpolation endpoints because they never update the last-valid state.
class GapDetector {
public:
    struct MissingInterval {
        std::string device_id;
        std::string metric_id;
        int64_t start_us = 0;
        int64_t end_us = 0;
        int64_t expected_count = 0;
    };

    using SampleFn = std::function<void(NormalizedSample&&)>;
    using IntervalFn = std::function<void(const MissingInterval&)>;

    GapDetector(const ConfigSnapshot& cs, SampleFn sample_out, IntervalFn interval_out);

    // Feeds one sample; valid samples drive gap detection and interpolation.
    void feed(const NormalizedSample& sample);

    // Re-seeds the last-valid state from the database (crash recovery / restart):
    // (event_time, value) of the newest non-null, non-device-error sample of the series.
    Result<bool> seed_from_store(storage::Store& store, const std::string& device_id, const std::string& metric_id);

private:
    struct LastValid {
        int64_t time_us = 0;
        double value = 0.0;
    };

    // Returns nullptr when the metric has no expected period configured (detection off).
    [[nodiscard]] const MetricCfg* metric_cfg(const std::string& device_id, const std::string& metric_id) const;

    const ConfigSnapshot* cs_;
    SampleFn sample_out_;
    IntervalFn interval_out_;
    std::map<std::pair<std::string, std::string>, LastValid> last_valid_;
};

} // namespace processing
} // namespace streamforge
