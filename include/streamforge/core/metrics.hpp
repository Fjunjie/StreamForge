#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace streamforge {

// Central in-process metrics registry (requirement FR-OBS-002). Counters are
// monotonically increasing, gauges hold the last set value; both support one label set
// each (e.g. http_status="200"). All operations are thread-safe and O(1) amortized.
// render_prometheus() emits the Prometheus text exposition format (v0.0.4).
class MetricsRegistry {
public:
    using Labels = std::vector<std::pair<std::string, std::string>>;

    static MetricsRegistry& instance();

    // Adds delta (default 1) to the counter. Creates it when new.
    void inc_counter(const std::string& name, const Labels& labels = {}, double delta = 1.0);
    // Sets the gauge value.
    void set_gauge(const std::string& name, const Labels& labels = {}, double value = 0.0);

    // Current values (test hooks / internal use).
    [[nodiscard]] double counter(const std::string& name, const Labels& labels = {}) const;
    [[nodiscard]] double gauge(const std::string& name, const Labels& labels = {}) const;

    // Prometheus text format: one "# TYPE" header per metric family, then one line per
    // label set with labels rendered as name="value" pairs.
    [[nodiscard]] std::string render_prometheus() const;

    // Drops all series (tests).
    void reset();

private:
    MetricsRegistry() = default;

    struct Series {
        bool is_gauge = false;
        double value = 0.0;
    };

    static std::string series_key(const std::string& name, const Labels& labels);
    static std::string escape_label_value(const std::string& v);
    static std::string render_labels(const Labels& labels);

    mutable std::mutex mu_;
    // key -> name (family), label text as rendered, value, kind
    struct Entry {
        std::string name;
        std::string label_text; // empty or `{a="b",c="d"}`
        Series series;
    };
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace streamforge
