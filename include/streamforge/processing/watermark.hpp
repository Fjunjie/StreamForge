#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "streamforge/processing/types.hpp"

namespace streamforge {
namespace processing {

// Per-device reorder buffer with event-time watermark (requirement FR-ORD-002).
//
//   watermark(device) = max_seen_event_time(device) - allowed_lateness
//
// Samples newer than the watermark are buffered and emitted in (event_time, sequence,
// arrival) order as the watermark advances. Samples older than the watermark at arrival
// are late: they are emitted immediately with the kLate flag and are not buffered.
// The buffer has record-count and byte-size caps; when either is hit the watermark is
// force-advanced to max_seen (all buffered samples emitted, kForcedFlush flag set).
class ReorderBuffer {
public:
    using EmitFn = std::function<void(NormalizedSample&&)>;

    ReorderBuffer(int64_t allowed_lateness_us, size_t max_records, size_t max_bytes, EmitFn emit);

    // Buffers or emits (as late) the sample; drains everything the new watermark releases.
    void push(NormalizedSample sample);

    // Seeds a device's max-seen watermark from durable history (cross-file lateness).
    void seed_device(const std::string& device, int64_t max_seen_us);

    // Force-advances the watermark for `device` to its max seen event time and emits all
    // buffered samples. mark_forced controls whether the kForcedFlush flag is set: true
    // for capacity-driven flushes, false for the regular end-of-file drain.
    void flush_device(const std::string& device, bool mark_forced = true);

    // Current watermark for a device (INT64_MIN before the first sample).
    [[nodiscard]] int64_t watermark(const std::string& device) const;
    // True when at least one sample has been seen for the device.
    [[nodiscard]] bool has_watermark(const std::string& device) const;
    [[nodiscard]] size_t buffered_records(const std::string& device) const;

private:
    struct Entry {
        NormalizedSample sample;
        uint64_t arrival;
    };

    struct DeviceBuffer {
        bool has_max = false;
        int64_t max_seen = 0;
        uint64_t next_arrival = 0;
        size_t buffered_records = 0;
        size_t buffered_bytes = 0;
        // keyed by event time; per-time vectors keep arrival order and are sorted by
        // (sequence, arrival) when drained.
        std::map<int64_t, std::vector<Entry>> by_time;
    };

    void drain(DeviceBuffer& buf, const std::string& device, int64_t up_to_watermark, bool forced);
    static size_t sample_bytes(const NormalizedSample& s);

    int64_t allowed_lateness_us_;
    size_t max_records_;
    size_t max_bytes_;
    EmitFn emit_;
    std::map<std::string, DeviceBuffer> devices_;
};

} // namespace processing
} // namespace streamforge
