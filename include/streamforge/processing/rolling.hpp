#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>

namespace streamforge {
namespace processing {

// Incrementally maintained rolling statistics (requirement FR-AGG-002): mean and stddev
// via running sums, evicting samples that fall out of the window (by count or duration).
// min/max are recomputed on demand over the bounded deque — the requirement rules out
// scanning database history, not O(window) memory work.
class RollingSeries {
public:
    // window_count > 0: keep the newest window_count samples.
    // window_duration_us > 0: additionally drop samples older than newest - duration.
    RollingSeries(int64_t window_count, int64_t window_duration_us)
        : window_count_(window_count), window_duration_us_(window_duration_us) {}

    void add(int64_t time_us, double value) {
        entries_.push_back(Entry{time_us, value});
        sum_ += value;
        sum_sq_ += value * value;
        prune(time_us);
    }

    void prune(int64_t newest_time_us) {
        while (!entries_.empty() &&
               ((window_count_ > 0 && static_cast<int64_t>(entries_.size()) > window_count_) ||
                (window_duration_us_ > 0 && newest_time_us - entries_.front().time_us > window_duration_us_))) {
            const Entry& front = entries_.front();
            sum_ -= front.value;
            sum_sq_ -= front.value * front.value;
            entries_.pop_front();
        }
    }

    [[nodiscard]] bool ready(std::optional<int64_t> min_count = std::nullopt) const {
        int64_t needed = min_count.value_or(1);
        return static_cast<int64_t>(entries_.size()) >= needed;
    }

    [[nodiscard]] size_t size() const { return entries_.size(); }
    [[nodiscard]] double mean() const { return sum_ / static_cast<double>(entries_.size()); }
    [[nodiscard]] double min() const {
        double m = entries_.front().value;
        for (const auto& e : entries_)
            m = e.value < m ? e.value : m;
        return m;
    }
    [[nodiscard]] double max() const {
        double m = entries_.front().value;
        for (const auto& e : entries_)
            m = e.value > m ? e.value : m;
        return m;
    }
    // Population standard deviation via E[x^2] - E[x]^2 on the running sums.
    [[nodiscard]] double stddev() const {
        if (entries_.size() < 2)
            return 0.0;
        double n = static_cast<double>(entries_.size());
        double mean = sum_ / n;
        double variance = sum_sq_ / n - mean * mean;
        return variance > 0.0 ? std::sqrt(variance) : 0.0;
    }
    // Rate of change between the newest and oldest retained samples, per second.
    [[nodiscard]] double rate_per_second() const {
        if (entries_.size() < 2)
            return 0.0;
        double dt = static_cast<double>(entries_.back().time_us - entries_.front().time_us) / 1e6;
        if (dt <= 0.0)
            return 0.0;
        return (entries_.back().value - entries_.front().value) / dt;
    }
    [[nodiscard]] double newest() const { return entries_.back().value; }
    [[nodiscard]] double oldest() const { return entries_.front().value; }
    [[nodiscard]] int64_t newest_time() const { return entries_.back().time_us; }

private:
    struct Entry {
        int64_t time_us;
        double value;
    };

    int64_t window_count_;
    int64_t window_duration_us_;
    std::deque<Entry> entries_;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
};

} // namespace processing
} // namespace streamforge
