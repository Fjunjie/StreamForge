#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"
#include "streamforge/processing/types.hpp"
#include "streamforge/storage/store.hpp"

namespace streamforge {
namespace processing {

// Fixed-window aggregation over the samples table (requirements FR-AGG-001/FR-AGG-003).
//
// Design: the samples table is the durable state. A window is CLOSED by recomputing its
// statistics with one indexed query over [start, end) — never by accumulating in-memory
// state — which makes closing idempotent and crash-safe. Windows are tracked in memory
// only while open (between the first sample and the watermark passing their end).
//
// Window identity: (device_id, metric_id, window_type, start_us, version); window_type is
// "<seconds>s". Closing writes version 1 unless that version exists and was superseded —
// in which case the next free version is appended. Late-data correction (when
// pipeline.window_correction is enabled) supersedes existing versions and writes a new
// one; with correction disabled a closed window is left untouched and the late samples
// are only stored.
class WindowAggregator {
public:
    WindowAggregator(const ConfigSnapshot& cs, storage::Store& store);

    // "60s" style window type identifier used in the aggregates table.
    static std::string window_type_text(int64_t seconds);

    // Registers the affected windows of an emitted sample.
    void on_sample(const NormalizedSample& sample);

    // Closes every open window of `device` whose end_us <= watermark. Called when the
    // device watermark advances.
    Result<void> on_watermark(const std::string& device_id, int64_t watermark);

    // Closes every still-open window of `device` regardless of the watermark; called at
    // end-of-file so batch imports persist complete results.
    Result<void> close_all(const std::string& device_id);

    // Late correction for one sample's affected windows (FR-AGG-003): recomputes each
    // window and appends a new version, marking older versions superseded. No-op when
    // pipeline.window_correction is disabled.
    Result<void> correct_for_sample(const NormalizedSample& sample);

    struct WindowKey {
        std::string device_id;
        std::string metric_id;
        int64_t window_seconds;
        int64_t start_us;
        bool operator<(const WindowKey& o) const {
            if (device_id != o.device_id)
                return device_id < o.device_id;
            if (metric_id != o.metric_id)
                return metric_id < o.metric_id;
            if (window_seconds != o.window_seconds)
                return window_seconds < o.window_seconds;
            return start_us < o.start_us;
        }
    };

private:
    static int64_t align_down(int64_t t_us, int64_t window_seconds);
    // True when the key is known closed (memory cache or the aggregates table). Closed
    // windows only change via correct_for_sample.
    bool is_closed(const WindowKey& key);
    // Audit #13: bounds the closed-window cache — it is a lookup accelerator only;
    // is_closed() falls back to the database after eviction.
    void evict_closed_cache();
    Result<void> close_window(const WindowKey& key, int64_t end_us);
    Result<int64_t> next_version(const WindowKey& key);

    const ConfigSnapshot* cs_;
    storage::Store* store_;
    std::map<std::string, std::set<WindowKey>> open_windows_; // per device
    std::set<WindowKey> closed_;
};

} // namespace processing
} // namespace streamforge
