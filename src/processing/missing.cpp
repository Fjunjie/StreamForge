#include "streamforge/processing/missing.hpp"

#include <cmath>

#include "streamforge/core/types.hpp"

namespace streamforge {
namespace processing {

GapDetector::GapDetector(const ConfigSnapshot& cs, SampleFn sample_out, IntervalFn interval_out)
    : cs_(&cs), sample_out_(std::move(sample_out)), interval_out_(std::move(interval_out)) {}

const MetricCfg* GapDetector::metric_cfg(const std::string& device_id, const std::string& metric_id) const {
    const MetricCfg* metric = cs_->metric(metric_id);
    if (metric == nullptr || !metric->expected_period_us)
        return nullptr;
    // The metric must be allowed for this device; otherwise no series context exists.
    const DeviceCfg* dev = cs_->device(device_id);
    if (dev == nullptr)
        return nullptr;
    return metric;
}

Result<bool> GapDetector::seed_from_store(storage::Store& store, const std::string& device_id,
                                          const std::string& metric_id) {
    // Newest non-null, non-device-error sample of the series becomes the last-valid state;
    // false when the series has no such sample yet.
    auto st = store.db().prepare("SELECT event_time_us, value FROM samples WHERE device_id=? AND metric_id=? AND"
                                 " value IS NOT NULL AND quality != 2 ORDER BY event_time_us DESC LIMIT 1");
    if (!st.ok())
        return Result<bool>::Err(st.error());
    st.value().bind_text(1, device_id);
    st.value().bind_text(2, metric_id);
    auto step = st.value().step();
    if (!step.ok())
        return Result<bool>::Err(step.error());
    if (step.value() != storage::Stmt::Step::Row)
        return Result<bool>::Ok(false);
    LastValid lv;
    lv.time_us = st.value().column_int64(0);
    lv.value = st.value().column_double(1);
    last_valid_[{device_id, metric_id}] = lv;
    return Result<bool>::Ok(true);
}

void GapDetector::feed(const NormalizedSample& sample) {
    // Device-error samples never drive gap detection (no valid endpoints).
    if (sample.value_is_null || sample.quality == static_cast<int>(Quality::DeviceError)) {
        return;
    }
    const MetricCfg* metric = metric_cfg(sample.device_id, sample.metric_id);
    if (metric == nullptr)
        return;

    auto key = std::make_pair(sample.device_id, sample.metric_id);
    auto it = last_valid_.find(key);
    if (it == last_valid_.end()) {
        last_valid_[key] = LastValid{sample.event_time_us, sample.value};
        return;
    }

    const LastValid prev = it->second;
    const int64_t gap = sample.event_time_us - prev.time_us;
    if (gap <= 0) {
        // Out-of-order or duplicate arrival: keep the newest state.
        if (sample.event_time_us > prev.time_us) {
            it->second = LastValid{sample.event_time_us, sample.value};
        }
        return;
    }

    const int64_t period = metric->expected_period_us.value_or(0);
    if (period <= 0)
        return;
    const int64_t threshold = period + metric->jitter_us.value_or(0);
    if (gap > threshold) {
        MissingInterval interval;
        interval.device_id = sample.device_id;
        interval.metric_id = sample.metric_id;
        interval.start_us = prev.time_us;
        interval.end_us = sample.event_time_us;
        interval.expected_count = gap / period;
        if (interval.expected_count > 0)
            interval.expected_count -= 1;
        interval_out_(interval);

        const int64_t max_gap = metric->max_gap_us.value_or(INT64_MAX);
        if (metric->interpolation != Interpolation::None && gap <= max_gap) {
            // Interior points at expected-period spacing; endpoints are prev and current.
            for (int64_t t = prev.time_us + period; t < sample.event_time_us; t += period) {
                NormalizedSample synth;
                synth.device_id = sample.device_id;
                synth.metric_id = sample.metric_id;
                synth.event_time_us = t;
                synth.ingest_time_us = sample.ingest_time_us;
                synth.quality = static_cast<int>(Quality::Normal);
                synth.flags |= sample_flags::kSynthetic;
                synth.tags_json = "{}";
                if (metric->interpolation == Interpolation::Previous) {
                    synth.value = prev.value;
                } else { // Linear between the two valid endpoints.
                    double ratio = static_cast<double>(t - prev.time_us) /
                                   static_cast<double>(sample.event_time_us - prev.time_us);
                    synth.value = prev.value + (sample.value - prev.value) * ratio;
                }
                sample_out_(std::move(synth));
            }
        }
    }
    it->second = LastValid{sample.event_time_us, sample.value};
}

} // namespace processing
} // namespace streamforge
