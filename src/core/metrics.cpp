#include "streamforge/core/metrics.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

namespace streamforge {

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry registry;
    return registry;
}

std::string MetricsRegistry::series_key(const std::string& name, const Labels& labels) {
    if (labels.empty())
        return name;
    std::string key = name;
    for (const auto& [k, v] : labels) {
        key += '|';
        key += k;
        key += '=';
        key += v;
    }
    return key;
}

std::string MetricsRegistry::escape_label_value(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        if (c == '\\' || c == '"' || c == '\n')
            out += '\\';
        out += c;
    }
    return out;
}

std::string MetricsRegistry::render_labels(const Labels& labels) {
    if (labels.empty())
        return {};
    std::string out = "{";
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0)
            out += ',';
        out += labels[i].first;
        out += "=\"";
        out += escape_label_value(labels[i].second);
        out += '"';
    }
    out += '}';
    return out;
}

void MetricsRegistry::inc_counter(const std::string& name, const Labels& labels, double delta) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& e = entries_[series_key(name, labels)];
    e.name = name;
    e.label_text = render_labels(labels);
    e.series.is_gauge = false;
    e.series.value += delta;
}

void MetricsRegistry::set_gauge(const std::string& name, const Labels& labels, double value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& e = entries_[series_key(name, labels)];
    e.name = name;
    e.label_text = render_labels(labels);
    e.series.is_gauge = true;
    e.series.value = value;
}

double MetricsRegistry::counter(const std::string& name, const Labels& labels) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(series_key(name, labels));
    return it == entries_.end() || it->second.series.is_gauge ? 0.0 : it->second.series.value;
}

double MetricsRegistry::gauge(const std::string& name, const Labels& labels) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(series_key(name, labels));
    return it == entries_.end() || !it->second.series.is_gauge ? 0.0 : it->second.series.value;
}

std::string MetricsRegistry::render_prometheus() const {
    std::lock_guard<std::mutex> lk(mu_);

    // Group series by metric family and sort the whole output by family name so the
    // exposition is stable across scrapes (labels keep their insertion order within
    // a series, but series keys are sorted for determinism).
    std::map<std::string, std::vector<const Entry*>> families;
    for (const auto& [key, entry] : entries_)
        families[entry.name].push_back(&entry);

    std::ostringstream out;
    for (auto& [name, list] : families) {
        std::sort(list.begin(), list.end(), [](const Entry* a, const Entry* b) {
            const std::string& ka = a->label_text;
            const std::string& kb = b->label_text;
            return ka < kb;
        });
        out << "# TYPE " << name << ' ' << (list.front()->series.is_gauge ? "gauge" : "counter") << '\n';
        for (const Entry* e : list)
            out << e->name << e->label_text << ' ' << e->series.value << '\n';
    }
    return out.str();
}

void MetricsRegistry::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
}

} // namespace streamforge
