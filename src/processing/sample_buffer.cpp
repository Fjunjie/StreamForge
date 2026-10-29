#include "streamforge/processing/sample_buffer.hpp"

#include <algorithm>
#include <cmath>

namespace streamforge {
namespace processing {

void SampleBuffer::add(const std::string& device, const std::string& metric, int64_t time_us, double value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& deque = series_[std::make_pair(device, metric)];
    // Late samples would corrupt the newest-anchored windows; keep the ring moving
    // forward like DerivedCalculator's state.
    if (!deque.empty() && time_us < deque.back().time_us)
        return;
    deque.push_back(Entry{time_us, value});
    while (!deque.empty() &&
           ((max_window_us_ > 0 && deque.back().time_us - deque.front().time_us > max_window_us_) ||
            deque.size() > max_samples_))
        deque.pop_front();
}

std::optional<SampleBuffer::Latest> SampleBuffer::latest(const std::string& device,
                                                         const std::string& metric) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = series_.find(std::make_pair(device, metric));
    if (it == series_.end() || it->second.empty())
        return std::nullopt;
    return latest_entry(it->second);
}

std::map<std::string, SampleBuffer::Latest> SampleBuffer::latest_by_metric(const std::string& device) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, Latest> out;
    for (const auto& [key, deque] : series_) {
        if (key.first == device && !deque.empty())
            out[key.second] = latest_entry(deque);
    }
    return out;
}

std::optional<double> SampleBuffer::window(const std::string& device, const std::string& metric,
                                           int64_t duration_us, const std::string& func) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = series_.find(std::make_pair(device, metric));
    if (it == series_.end() || it->second.empty())
        return std::nullopt;
    const auto& deque = it->second;
    const int64_t newest_time = deque.back().time_us;
    const int64_t window_start = newest_time - duration_us;

    double sum = 0.0;
    size_t count = 0;
    const Entry* oldest_in_window = nullptr;
    for (const auto& e : deque) {
        if (e.time_us < window_start)
            continue;
        sum += e.value;
        ++count;
        if (oldest_in_window == nullptr)
            oldest_in_window = &e;
    }
    if (count == 0)
        return std::nullopt;

    if (func == "avg")
        return sum / static_cast<double>(count);
    if (count < 2)
        return std::nullopt; // delta and rate need two samples
    const double delta = deque.back().value - oldest_in_window->value;
    if (func == "delta")
        return delta;
    if (func == "rate") {
        const double dt = static_cast<double>(deque.back().time_us - oldest_in_window->time_us) / 1e6;
        if (dt <= 0.0)
            return std::nullopt;
        return delta / dt;
    }
    return std::nullopt; // unknown function name
}

size_t SampleBuffer::size(const std::string& device, const std::string& metric) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = series_.find(std::make_pair(device, metric));
    return it == series_.end() ? 0 : it->second.size();
}

} // namespace processing
} // namespace streamforge
