#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace streamforge {
namespace processing {

// Time-bounded in-memory ring of recent samples per (device, metric). Feeds rule window
// functions — AVG(metric, duration), DELTA(metric, duration), RATE(metric, duration) —
// by aggregating over the bounded buffer instead of scanning database history
// (requirement FR-AGG-002). Thread-safe: the ingest thread adds samples while the HTTP
// and rule paths read.
class SampleBuffer {
public:
    struct Latest {
        double value = 0.0;
        int64_t time_us = 0;
    };

    // max_window_us bounds retention by event time (the largest rule window duration);
    // max_samples bounds retention by count as a memory guard.
    SampleBuffer(int64_t max_window_us, size_t max_samples = 4096)
        : max_window_us_(max_window_us), max_samples_(max_samples) {}

    void add(const std::string& device, const std::string& metric, int64_t time_us, double value);

    [[nodiscard]] std::optional<Latest> latest(const std::string& device, const std::string& metric) const;
    // Latest value of every metric known for the device (rule evaluation snapshot).
    [[nodiscard]] std::map<std::string, Latest> latest_by_metric(const std::string& device) const;

    // Aggregates the trailing duration_us relative to the newest retained sample.
    //   avg   - mean over the samples inside the window (>= 1 sample required)
    //   delta - newest minus oldest sample inside the window (>= 2 required)
    //   rate  - delta per second over the window's time span (>= 2 required)
    // nullopt when not enough data falls inside the window.
    [[nodiscard]] std::optional<double> window(const std::string& device, const std::string& metric,
                                               int64_t duration_us, const std::string& func) const;

    [[nodiscard]] size_t size(const std::string& device, const std::string& metric) const;

private:
    struct Entry {
        int64_t time_us;
        double value;
    };

    static Latest latest_entry(const std::deque<Entry>& deque) {
        return Latest{deque.back().value, deque.back().time_us};
    }

    mutable std::mutex mu_;
    int64_t max_window_us_;
    size_t max_samples_;
    std::map<std::pair<std::string, std::string>, std::deque<Entry>> series_;
};

} // namespace processing
} // namespace streamforge
