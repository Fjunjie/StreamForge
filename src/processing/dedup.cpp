#include "streamforge/processing/dedup.hpp"

#include <cstdio>
#include <cstring>

#include "streamforge/core/types.hpp"

namespace streamforge {
namespace processing {

std::string normalized_value_text(bool value_is_null, double value) {
    if (value_is_null)
        return "null";
    // Deterministic bit pattern: raw IEEE 754 bits in lowercase hex (FR-ORD-001 forbids
    // tolerance-based keys). -0.0 and 0.0 normalize to one key.
    double normalized = value == 0.0 ? 0.0 : value;
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(normalized), "64-bit IEEE 754 doubles required");
    std::memcpy(&bits, &normalized, sizeof(bits));
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(bits));
    return {buf};
}

DedupIndex::DedupIndex(storage::Store& store, size_t cache_capacity) : store_(&store), capacity_(cache_capacity) {}

std::string DedupIndex::key_of(const NormalizedSample& s) const {
    // Key layout: device \0 metric \0 discriminator. With a sequence the key is
    // (device, metric, sequence); without one it is (device, metric, event_time_us,
    // normalized_value) — the normalized value is the raw IEEE 754 bit pattern in hex,
    // never a tolerance-based comparison.
    std::string key;
    key.reserve(s.device_id.size() + s.metric_id.size() + 40);
    key += s.device_id;
    key.push_back('\0');
    key += s.metric_id;
    key.push_back('\0');
    if (s.has_sequence) {
        key += "s:";
        key += std::to_string(s.sequence);
    } else {
        key += "v:";
        key += std::to_string(s.event_time_us);
        key.push_back('\0');
        key += normalized_value_text(s.value_is_null, s.value);
    }
    return key;
}

void DedupIndex::touch(const std::string& key) {
    auto it = index_.find(key);
    if (it != index_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    insert(key);
}

void DedupIndex::insert(const std::string& key) {
    lru_.push_front(key);
    index_[key] = lru_.begin();
    if (index_.size() > capacity_) {
        index_.erase(lru_.back());
        lru_.pop_back();
    }
}

Result<bool> DedupIndex::is_duplicate(const NormalizedSample& sample) {
    std::string key = key_of(sample);
    auto it = index_.find(key);
    if (it != index_.end()) {
        // Cache hit: already seen in this process, therefore present in the database.
        lru_.splice(lru_.begin(), lru_, it->second);
        return Result<bool>::Ok(true);
    }
    // Cache miss: SQLite is the authority (agreed design; no Bloom filter shortcuts).
    bool duplicate = false;
    if (sample.has_sequence) {
        auto st =
            store_->db().prepare("SELECT 1 FROM samples WHERE device_id=? AND metric_id=? AND sequence=? LIMIT 1");
        if (!st.ok())
            return Result<bool>::Err(st.error());
        st.value().bind_text(1, sample.device_id);
        st.value().bind_text(2, sample.metric_id);
        st.value().bind_int64(3, static_cast<int64_t>(sample.sequence));
        auto step = st.value().step();
        if (!step.ok())
            return Result<bool>::Err(step.error());
        duplicate = step.value() == storage::Stmt::Step::Row;
    } else {
        auto st = store_->db().prepare("SELECT 1 FROM samples WHERE device_id=? AND metric_id=? AND event_time_us=? AND"
                                       " normalized_value=? LIMIT 1");
        if (!st.ok())
            return Result<bool>::Err(st.error());
        st.value().bind_text(1, sample.device_id);
        st.value().bind_text(2, sample.metric_id);
        st.value().bind_int64(3, sample.event_time_us);
        st.value().bind_text(4, normalized_value_text(sample.value_is_null, sample.value));
        auto step = st.value().step();
        if (!step.ok())
            return Result<bool>::Err(step.error());
        duplicate = step.value() == storage::Stmt::Step::Row;
    }
    insert(key);
    return Result<bool>::Ok(duplicate);
}

void DedupIndex::mark_seen(const NormalizedSample& sample) {
    touch(key_of(sample));
}

} // namespace processing
} // namespace streamforge
