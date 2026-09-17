#include "streamforge/processing/watermark.hpp"

#include <algorithm>

#include "streamforge/core/types.hpp"

namespace streamforge {
namespace processing {

ReorderBuffer::ReorderBuffer(int64_t allowed_lateness_us, size_t max_records, size_t max_bytes, EmitFn emit)
    : allowed_lateness_us_(allowed_lateness_us), max_records_(max_records), max_bytes_(max_bytes),
      emit_(std::move(emit)) {}

size_t ReorderBuffer::sample_bytes(const NormalizedSample& s) {
    return sizeof(NormalizedSample) + s.device_id.capacity() + s.metric_id.capacity() + s.tags_json.capacity();
}

void ReorderBuffer::push(NormalizedSample sample) {
    DeviceBuffer& buf = devices_[sample.device_id];
    if (!buf.has_max || sample.event_time_us > buf.max_seen) {
        buf.max_seen = sample.event_time_us;
        buf.has_max = true;
    }
    const int64_t watermark = buf.max_seen - allowed_lateness_us_;
    if (sample.event_time_us < watermark) {
        // Late arrival: still stored as a sample, but flagged and never buffered.
        sample.flags |= sample_flags::kLate;
        emit_(std::move(sample));
        return;
    }

    const std::string device_id = sample.device_id;
    Entry entry{std::move(sample), buf.next_arrival++};
    buf.buffered_records += 1;
    buf.buffered_bytes += sample_bytes(entry.sample);
    buf.by_time[entry.sample.event_time_us].push_back(std::move(entry));

    // Normal watermark drain first (audit #18): on-time samples are released without
    // the forced flag; only the over-capacity remainder is force-flushed.
    drain(buf, device_id, watermark, false);
    if (buf.buffered_records >= max_records_ || buf.buffered_bytes >= max_bytes_) {
        drain(buf, device_id, buf.max_seen, true);
    }
}

void ReorderBuffer::seed_device(const std::string& device, int64_t max_seen_us) {
    DeviceBuffer& buf = devices_[device];
    if (!buf.has_max || max_seen_us > buf.max_seen) {
        buf.max_seen = max_seen_us;
        buf.has_max = true;
    }
}

void ReorderBuffer::flush_device(const std::string& device, bool mark_forced) {
    auto it = devices_.find(device);
    if (it == devices_.end())
        return;
    if (it->second.has_max) {
        drain(it->second, device, it->second.max_seen, mark_forced);
    }
}

void ReorderBuffer::drain(DeviceBuffer& buf, const std::string& device, int64_t up_to_watermark, bool forced) {
    // Everything with event_time <= up_to_watermark is released in (event_time, sequence,
    // arrival) order.
    auto it = buf.by_time.begin();
    while (it != buf.by_time.end() && it->first <= up_to_watermark) {
        std::vector<Entry> group = std::move(it->second);
        it = buf.by_time.erase(it);
        std::sort(group.begin(), group.end(), [](const Entry& a, const Entry& b) {
            const bool a_seq = a.sample.has_sequence;
            const bool b_seq = b.sample.has_sequence;
            if (a_seq != b_seq)
                return a_seq; // sequenced samples order before unsequenced
            if (a_seq && a.sample.sequence != b.sample.sequence) {
                return a.sample.sequence < b.sample.sequence;
            }
            return a.arrival < b.arrival;
        });
        for (auto& entry : group) {
            if (forced)
                entry.sample.flags |= sample_flags::kForcedFlush;
            buf.buffered_records -= 1;
            buf.buffered_bytes -= sample_bytes(entry.sample);
            emit_(std::move(entry.sample));
        }
    }
    (void)device;
}

int64_t ReorderBuffer::watermark(const std::string& device) const {
    auto it = devices_.find(device);
    if (it == devices_.end() || !it->second.has_max)
        return INT64_MIN;
    return it->second.max_seen - allowed_lateness_us_;
}

bool ReorderBuffer::has_watermark(const std::string& device) const {
    auto it = devices_.find(device);
    return it != devices_.end() && it->second.has_max;
}

size_t ReorderBuffer::buffered_records(const std::string& device) const {
    auto it = devices_.find(device);
    return it == devices_.end() ? 0u : it->second.buffered_records;
}

} // namespace processing
} // namespace streamforge
