#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "streamforge/core/error.hpp"
#include "streamforge/core/log.hpp"

namespace date {
class time_zone;
}

namespace streamforge {

enum class Interpolation { None, Previous, Linear };
enum class AmbiguousTimePolicy { Earlier, Later };

struct CalibrationSegment {
    double min_inclusive = 0.0; // raw value range [min, max)
    double max_exclusive = 0.0;
    double slope = 1.0;
    double intercept = 0.0;
};

struct Calibration {
    bool reject_unmatched = false; // no matching segment: reject the sample instead of passthrough
    std::vector<CalibrationSegment> segments;
};

struct DeviceCalibration {
    std::string device_id;
    std::string metric_id;
    Calibration calibration;
};

struct MetricCfg {
    std::string id;
    std::string canonical_unit;
    std::vector<std::string> input_units;
    std::optional<double> valid_min;
    std::optional<double> valid_max;
    std::optional<int64_t> expected_period_us;
    std::optional<int64_t> jitter_us;
    std::optional<int64_t> max_gap_us;
    Interpolation interpolation = Interpolation::None;
};

struct DeviceCfg {
    std::string id;
    std::string timezone = "UTC";
    std::vector<std::pair<std::string, std::string>> tags;
    std::vector<std::string> metrics; // allowed metric ids; empty means all
};

struct DerivedMetricCfg {
    std::string id;
    std::string unit;
    std::string expression;
    std::string missing_policy = "null"; // null|recent|skip
    int64_t max_input_age_us = 0;
};

struct RuleCfg {
    std::string id;
    std::string type;                                         // threshold|duration|rate|absence|composite
    std::string severity;                                     // info|warning|high|critical
    std::string trigger;                                      // expression evaluated on each sample (M3)
    std::string recovery;                                     // optional recovery expression (M3)
    int64_t duration_us = 0;                                  // condition sustain time (0 = immediate)
    int64_t cooldown_us = 0;                                  // suppression after close
    int64_t merge_interval_us = 0;                            // reopen window for recently-closed incidents
    std::vector<std::pair<std::string, std::string>> devices; // tag selector
};

struct ServerConfig {
    std::string bind = "127.0.0.1";
    int port = 8080;
};

struct DirectoriesConfig {
    std::string input;
    std::string archive;
    std::string quarantine;
    std::string reports;
};

struct DatabaseConfig {
    std::string path;
};

struct PipelineConfig {
    int workers = 4;
    int64_t queue_capacity = 100000;
    int64_t batch_size = 1000;
    int64_t allowed_lateness_us = 300LL * 1000000; // 5 minutes
    int64_t scan_interval_ms = 2000;
    int64_t quiet_period_ms = 5000;
    double max_error_rate = 0.10;
    int64_t error_rate_min_records = 100;
    int64_t shutdown_timeout_us = 25LL * 1000000;
    int64_t history_range_us = 7LL * 24 * 3600 * 1000000;
    AmbiguousTimePolicy ambiguous_time_policy = AmbiguousTimePolicy::Earlier;
    bool window_correction = false; // FR-AGG-003: late data reopens closed windows with a new version
};

struct RetentionConfig {
    int64_t samples_days = 90;
    int64_t aggregates_days = 365;
    int64_t incidents_days = 730;
    int64_t audit_days = 365;
};

struct Config {
    int schema_version = 1;
    ServerConfig server;
    DirectoriesConfig directories;
    DatabaseConfig database;
    PipelineConfig pipeline;
    std::vector<int64_t> windows; // fixed-window sizes in seconds (semantics land in M2)
    LogConfig log;
    std::vector<MetricCfg> metrics;
    std::vector<DeviceCfg> devices;
    std::vector<DeviceCalibration> calibrations;
    std::vector<DerivedMetricCfg> derived_metrics;
    std::vector<RuleCfg> rules;
    RetentionConfig retention;
    std::string base_dir; // directory of the config file; relative paths resolve against it

    [[nodiscard]] const MetricCfg* metric(const std::string& id) const;
    [[nodiscard]] const DeviceCfg* device(const std::string& id) const;
};

// Immutable configuration handed to processing stages. Resolved timezones live here so that
// validation and runtime share one lookup.
struct ConfigSnapshot {
    Config cfg;
    uint64_t version = 0; // config_version recorded on files and samples
    std::map<std::string, const date::time_zone*> device_timezones;
    const date::time_zone* default_timezone = nullptr;

    [[nodiscard]] const MetricCfg* metric(const std::string& id) const { return cfg.metric(id); }
    [[nodiscard]] const DeviceCfg* device(const std::string& id) const { return cfg.device(id); }
    [[nodiscard]] const date::time_zone* device_tz(const std::string& device_id) const;
    [[nodiscard]] bool metric_allowed_for_device(const DeviceCfg& dev, const std::string& metric_id) const;
};

// Loads, validates and resolves a YAML configuration file (FR-CFG-001/002).
// Diagnostics warnings surface unknown optional fields.
Result<std::shared_ptr<const ConfigSnapshot>> load_config(const std::string& path);

// FNV-1a 64 hash over a canonical serialization of the configuration; used as config_version.
uint64_t compute_config_version(const Config& cfg);

// Parses "500ms" / "10s" / "5m" / "2h" / "7d" into microseconds.
Result<int64_t> parse_duration_us(const std::string& s);

} // namespace streamforge
