#include "streamforge/config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "streamforge/core/time.hpp"
#include "streamforge/core/units.hpp"

namespace streamforge {

const MetricCfg* Config::metric(const std::string& id) const {
    for (const auto& m : metrics) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

const DeviceCfg* Config::device(const std::string& id) const {
    for (const auto& d : devices) {
        if (d.id == id)
            return &d;
    }
    return nullptr;
}

const date::time_zone* ConfigSnapshot::device_tz(const std::string& device_id) const {
    auto it = device_timezones.find(device_id);
    return it != device_timezones.end() ? it->second : default_timezone;
}

bool ConfigSnapshot::metric_allowed_for_device(const DeviceCfg& dev, const std::string& metric_id) const {
    if (dev.metrics.empty())
        return true;
    return std::find(dev.metrics.begin(), dev.metrics.end(), metric_id) != dev.metrics.end();
}

Result<int64_t> parse_duration_us(const std::string& s) {
    if (s.empty()) {
        return Result<int64_t>::Err(Error::make(ErrorCode::ConfigValidation, "empty duration").ctx("value", s));
    }
    size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i]))))
        ++i;
    if (i == 0 || i == s.size()) {
        return Result<int64_t>::Err(
            Error::make(ErrorCode::ConfigValidation, "invalid duration, expected like \"10s\"").ctx("value", s));
    }
    int64_t number = 0;
    for (size_t k = 0; k < i; ++k)
        number = number * 10 + (s[k] - '0');
    std::string unit = s.substr(i);
    int64_t scale = 0;
    if (unit == "ms") {
        scale = 1000;
    } else if (unit == "s") {
        scale = 1000000;
    } else if (unit == "m") {
        scale = 60LL * 1000000;
    } else if (unit == "h") {
        scale = 3600LL * 1000000;
    } else if (unit == "d") {
        scale = 24LL * 3600 * 1000000;
    } else {
        return Result<int64_t>::Err(
            Error::make(ErrorCode::ConfigValidation, "invalid duration unit, expected ms|s|m|h|d").ctx("value", s));
    }
    return Result<int64_t>::Ok(number * scale);
}

namespace {

constexpr const char* kIdChars = "device/metric/rule/derived ids allow [A-Za-z0-9._-], length 1..64";

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64)
        return false;
    for (char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

bool valid_unit(const std::string& unit) {
    if (unit.empty() || unit.size() > 32)
        return false;
    for (char c : unit) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc >= 0x7F)
            return false;
    }
    return true;
}

// Collects unknown keys of a YAML mapping into warnings.
void check_keys(const YAML::Node& node, const std::vector<std::string>& allowed, const std::string& where,
                std::vector<std::string>& warnings) {
    if (!node.IsMap())
        return;
    for (const auto& item : node) {
        std::string key = item.first.as<std::string>();
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            std::string warning = "unknown field '";
            warning += key;
            warning += "' in ";
            warning += where;
            warnings.push_back(std::move(warning));
        }
    }
}

std::string require_string(const YAML::Node& parent, const std::string& key, const std::string& where,
                           std::vector<Error>& errors) {
    const YAML::Node n = parent[key];
    if (!n || n.IsNull()) {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "missing required field '" + key + "'").ctx("location", where));
        return {};
    }
    try {
        std::string v = n.as<std::string>();
        if (v.empty()) {
            errors.push_back(Error::make(ErrorCode::ConfigValidation, "field '" + key + "' must not be empty")
                                 .ctx("location", where));
        }
        return v;
    } catch (const YAML::TypedBadConversion<std::string>&) {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "field '" + key + "' must be a string").ctx("location", where));
        return {};
    }
}

std::string optional_string(const YAML::Node& parent, const std::string& key, const std::string& fallback) {
    const YAML::Node n = parent[key];
    if (!n || n.IsNull())
        return fallback;
    try {
        return n.as<std::string>();
    } catch (...) {
        return fallback;
    }
}

int64_t optional_int(const YAML::Node& parent, const std::string& key, int64_t fallback, const std::string& where,
                     std::vector<Error>& errors) {
    const YAML::Node n = parent[key];
    if (!n || n.IsNull())
        return fallback;
    try {
        return n.as<int64_t>();
    } catch (...) {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "field '" + key + "' must be an integer").ctx("location", where));
        return fallback;
    }
}

int64_t require_duration(const YAML::Node& parent, const std::string& key, const std::string& where,
                         std::vector<Error>& errors, int64_t fallback_us = -1) {
    const YAML::Node n = parent[key];
    if (!n || n.IsNull()) {
        if (fallback_us >= 0)
            return fallback_us;
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "missing required duration '" + key + "'").ctx("location", where));
        return -1;
    }
    std::string raw;
    try {
        raw = n.as<std::string>();
    } catch (...) {
        // A bare number is interpreted as seconds.
        try {
            return n.as<int64_t>() * 1000000;
        } catch (...) {
            errors.push_back(Error::make(ErrorCode::ConfigValidation, "field '" + key + "' must be a duration")
                                 .ctx("location", where));
            return -1;
        }
    }
    auto parsed = parse_duration_us(raw);
    if (!parsed.ok()) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "invalid duration for '" + key + "': " + raw)
                             .ctx("location", where));
        return -1;
    }
    return parsed.value();
}

std::optional<int64_t> optional_duration(const YAML::Node& parent, const std::string& key, const std::string& where,
                                         std::vector<Error>& errors) {
    const YAML::Node n = parent[key];
    if (!n || n.IsNull())
        return std::nullopt;
    auto v = require_duration(parent, key, where, errors, 0);
    if (v < 0 && errors.empty())
        return std::nullopt;
    if (v < 0)
        return std::nullopt;
    return v;
}

double require_number(const YAML::Node& parent, const std::string& key, const std::string& where,
                      std::vector<Error>& errors) {
    const YAML::Node n = parent[key];
    if (!n || n.IsNull()) {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "missing required field '" + key + "'").ctx("location", where));
        return 0.0;
    }
    try {
        double v = n.as<double>();
        if (!std::isfinite(v)) {
            errors.push_back(Error::make(ErrorCode::ConfigValidation, "field '" + key + "' must be a finite number")
                                 .ctx("location", where));
            return 0.0;
        }
        return v;
    } catch (...) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "field '" + key + "' must be a finite number")
                             .ctx("location", where));
        return 0.0;
    }
}

std::optional<double> optional_number(const YAML::Node& parent, const std::string& key, const std::string& where,
                                      std::vector<Error>& errors) {
    const YAML::Node n = parent[key];
    if (!n || n.IsNull())
        return std::nullopt;
    try {
        double v = n.as<double>();
        if (!std::isfinite(v)) {
            errors.push_back(
                Error::make(ErrorCode::ConfigValidation, "field '" + key + "' must be finite").ctx("location", where));
            return std::nullopt;
        }
        return v;
    } catch (...) {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "field '" + key + "' must be a number").ctx("location", where));
        return std::nullopt;
    }
}

} // namespace

uint64_t compute_config_version(const Config& cfg) {
    std::ostringstream canon;
    canon << "schema=" << cfg.schema_version << "|bind=" << cfg.server.bind << "|port=" << cfg.server.port
          << "|in=" << cfg.directories.input << "|arc=" << cfg.directories.archive
          << "|qtn=" << cfg.directories.quarantine << "|rpt=" << cfg.directories.reports << "|db=" << cfg.database.path;
    canon << "|w=" << cfg.pipeline.workers << ",q=" << cfg.pipeline.queue_capacity << ",b=" << cfg.pipeline.batch_size
          << ",lat=" << cfg.pipeline.allowed_lateness_us << ",scan=" << cfg.pipeline.scan_interval_ms
          << ",quiet=" << cfg.pipeline.quiet_period_ms << ",rate=" << cfg.pipeline.max_error_rate
          << ",minr=" << cfg.pipeline.error_rate_min_records << ",hist=" << cfg.pipeline.history_range_us
          << ",amb=" << (cfg.pipeline.ambiguous_time_policy == AmbiguousTimePolicy::Earlier ? "e" : "l")
          << ",wc=" << (cfg.pipeline.window_correction ? 1 : 0);
    canon << "|win=";
    for (auto w : cfg.windows)
        canon << w << ",";
    canon << "|log=" << cfg.log.level << "," << cfg.log.format << "," << cfg.log.file;
    for (const auto& m : cfg.metrics) {
        canon << "|M" << m.id << ":" << m.canonical_unit << ":";
        for (const auto& u : m.input_units)
            canon << u << ",";
        canon << ":ip=" << static_cast<int>(m.interpolation) << ",ep=" << m.expected_period_us.value_or(-1)
              << ",j=" << m.jitter_us.value_or(-1) << ",g=" << m.max_gap_us.value_or(-1)
              << ",vmin=" << m.valid_min.value_or(0) << ",vmax=" << m.valid_max.value_or(0);
    }
    for (const auto& d : cfg.devices) {
        canon << "|D" << d.id << ":" << d.timezone << ":m=";
        for (const auto& mid : d.metrics)
            canon << mid << ",";
        canon << ":t=";
        for (const auto& t : d.tags)
            canon << t.first << "=" << t.second << ",";
    }
    for (const auto& c : cfg.calibrations) {
        canon << "|C" << c.device_id << "/" << c.metric_id << "/r=" << c.calibration.reject_unmatched;
        for (const auto& s : c.calibration.segments) {
            canon << "[" << s.min_inclusive << "," << s.max_exclusive << ";" << s.slope << "+" << s.intercept << "]";
        }
    }
    for (const auto& dm : cfg.derived_metrics) {
        canon << "|X" << dm.id << ":" << dm.unit << ":" << dm.expression << ":" << dm.missing_policy << ":"
              << dm.max_input_age_us;
    }
    for (const auto& r : cfg.rules) {
        canon << "|R" << r.id << ":" << r.type << ":" << r.severity;
    }
    canon << "|ret=" << cfg.retention.samples_days << "," << cfg.retention.aggregates_days << ","
          << cfg.retention.incidents_days << "," << cfg.retention.audit_days;

    uint64_t hash = 1469598103934665603ULL; // FNV-1a 64 offset basis
    for (char c : canon.str()) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    if (hash == 0)
        hash = 1; // version 0 is reserved
    return hash;
}

namespace {

Result<std::shared_ptr<const ConfigSnapshot>> parse_and_validate(const YAML::Node& root, const std::string& base_dir) {
    std::vector<Error> errors;
    std::vector<std::string> warnings;

    auto cfg = std::make_shared<ConfigSnapshot>();
    Config& c = cfg->cfg;
    c.base_dir = base_dir;

    if (!root.IsMap()) {
        return Result<std::shared_ptr<const ConfigSnapshot>>::Err(
            Error::make(ErrorCode::ConfigParse, "configuration root must be a mapping"));
    }

    // schema_version (mandatory, major version gate).
    {
        const YAML::Node n = root["schema_version"];
        if (!n) {
            return Result<std::shared_ptr<const ConfigSnapshot>>::Err(
                Error::make(ErrorCode::ConfigValidation, "missing required field 'schema_version'")
                    .ctx("location", "root"));
        }
        int version = 0;
        try {
            version = n.as<int>();
        } catch (...) {
            return Result<std::shared_ptr<const ConfigSnapshot>>::Err(
                Error::make(ErrorCode::ConfigValidation, "'schema_version' must be an integer")
                    .ctx("location", "root"));
        }
        if (version != 1) {
            return Result<std::shared_ptr<const ConfigSnapshot>>::Err(
                Error::make(ErrorCode::ConfigSchemaUnsupported, "unsupported configuration schema major version " +
                                                                    std::to_string(version) + " (expected 1)")
                    .ctx("location", "root"));
        }
        c.schema_version = version;
    }

    check_keys(root,
               {"schema_version", "server", "directories", "database", "pipeline", "windows", "log", "metrics",
                "devices", "calibrations", "derived_metrics", "rules", "retention"},
               "root", warnings);

    // server ------------------------------------------------------------------
    if (const YAML::Node s = root["server"]) {
        check_keys(s, {"bind", "port"}, "server", warnings);
        c.server.bind = optional_string(s, "bind", c.server.bind);
        c.server.port = static_cast<int>(optional_int(s, "port", c.server.port, "server", errors));
    }
    if (c.server.bind.empty()) {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "server.bind must not be empty").ctx("location", "server"));
    }
    if (c.server.port < 1 || c.server.port > 65535) {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "server.port must be within 1..65535").ctx("location", "server"));
    }

    // directories ---------------------------------------------------------------
    if (const YAML::Node d = root["directories"]) {
        check_keys(d, {"input", "archive", "quarantine", "reports"}, "directories", warnings);
        c.directories.input = require_string(d, "input", "directories", errors);
        c.directories.archive = require_string(d, "archive", "directories", errors);
        c.directories.quarantine = require_string(d, "quarantine", "directories", errors);
        c.directories.reports = require_string(d, "reports", "directories", errors);
    } else {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "missing section 'directories'").ctx("location", "root"));
    }

    // database --------------------------------------------------------------
    if (const YAML::Node db = root["database"]) {
        check_keys(db, {"path"}, "database", warnings);
        c.database.path = require_string(db, "path", "database", errors);
    } else {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "missing section 'database'").ctx("location", "root"));
    }

    // pipeline -----------------------------------------------------------
    if (const YAML::Node p = root["pipeline"]) {
        check_keys(p,
                   {"workers", "queue_capacity", "batch_size", "allowed_lateness", "scan_interval", "quiet_period",
                    "max_error_rate", "error_rate_min_records", "shutdown_timeout", "history_range",
                    "ambiguous_time_policy", "window_correction"},
                   "pipeline", warnings);
        c.pipeline.workers = static_cast<int>(optional_int(p, "workers", c.pipeline.workers, "pipeline", errors));
        c.pipeline.queue_capacity = optional_int(p, "queue_capacity", c.pipeline.queue_capacity, "pipeline", errors);
        c.pipeline.batch_size = optional_int(p, "batch_size", c.pipeline.batch_size, "pipeline", errors);
        c.pipeline.allowed_lateness_us =
            require_duration(p, "allowed_lateness", "pipeline", errors, c.pipeline.allowed_lateness_us);
        c.pipeline.scan_interval_ms =
            require_duration(p, "scan_interval", "pipeline", errors, c.pipeline.scan_interval_ms * 1000) / 1000;
        c.pipeline.quiet_period_ms =
            require_duration(p, "quiet_period", "pipeline", errors, c.pipeline.quiet_period_ms * 1000) / 1000;
        c.pipeline.max_error_rate =
            optional_number(p, "max_error_rate", "pipeline", errors).value_or(c.pipeline.max_error_rate);
        c.pipeline.error_rate_min_records =
            optional_int(p, "error_rate_min_records", c.pipeline.error_rate_min_records, "pipeline", errors);
        c.pipeline.shutdown_timeout_us =
            require_duration(p, "shutdown_timeout", "pipeline", errors, c.pipeline.shutdown_timeout_us);
        c.pipeline.history_range_us =
            require_duration(p, "history_range", "pipeline", errors, c.pipeline.history_range_us);
        std::string amb = optional_string(p, "ambiguous_time_policy", "earlier");
        if (amb == "earlier") {
            c.pipeline.ambiguous_time_policy = AmbiguousTimePolicy::Earlier;
        } else if (amb == "later") {
            c.pipeline.ambiguous_time_policy = AmbiguousTimePolicy::Later;
        } else {
            errors.push_back(
                Error::make(ErrorCode::ConfigValidation, "pipeline.ambiguous_time_policy must be 'earlier' or 'later'")
                    .ctx("location", "pipeline")
                    .ctx("value", amb));
        }
        c.pipeline.window_correction = optional_string(p, "window_correction", "false") == "true";
    }
    if (c.pipeline.workers < 1 || c.pipeline.workers > 64) {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "pipeline.workers must be 1..64").ctx("location", "pipeline"));
    }
    if (c.pipeline.queue_capacity < 1 || c.pipeline.queue_capacity > 10000000) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "pipeline.queue_capacity must be 1..10000000")
                             .ctx("location", "pipeline"));
    }
    if (c.pipeline.batch_size < 1 || c.pipeline.batch_size > 100000) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "pipeline.batch_size must be 1..100000")
                             .ctx("location", "pipeline"));
    }
    if (c.pipeline.allowed_lateness_us <= 0) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "pipeline.allowed_lateness must be positive")
                             .ctx("location", "pipeline"));
    }
    if (c.pipeline.scan_interval_ms < 200) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "pipeline.scan_interval must be at least 200ms")
                             .ctx("location", "pipeline"));
    }
    if (c.pipeline.quiet_period_ms < 0) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "pipeline.quiet_period must not be negative")
                             .ctx("location", "pipeline"));
    }
    if (c.pipeline.max_error_rate <= 0.0 || c.pipeline.max_error_rate > 1.0) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "pipeline.max_error_rate must be within (0, 1]")
                             .ctx("location", "pipeline"));
    }
    if (c.pipeline.error_rate_min_records < 1) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "pipeline.error_rate_min_records must be at least 1")
                             .ctx("location", "pipeline"));
    }
    if (c.pipeline.shutdown_timeout_us < 1000000 || c.pipeline.shutdown_timeout_us > 120LL * 1000000) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "pipeline.shutdown_timeout must be 1s..120s")
                             .ctx("location", "pipeline"));
    }
    if (c.pipeline.history_range_us < 60000000) {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "pipeline.history_range must be at least 1 minute")
                             .ctx("location", "pipeline"));
    }

    // windows ----------------------------------------------------------
    if (const YAML::Node w = root["windows"]) {
        if (!w.IsSequence()) {
            errors.push_back(Error::make(ErrorCode::ConfigValidation, "'windows' must be a list of seconds")
                                 .ctx("location", "root"));
        } else {
            std::set<int64_t> seen;
            for (const auto& item : w) {
                try {
                    int64_t seconds = item.as<int64_t>();
                    if (seconds < 1 || seconds > 86400) {
                        errors.push_back(
                            Error::make(ErrorCode::ConfigValidation, "window size must be 1..86400 seconds")
                                .ctx("location", "windows"));
                    } else if (!seen.insert(seconds).second) {
                        warnings.push_back("duplicate window size " + std::to_string(seconds));
                    } else {
                        c.windows.push_back(seconds);
                    }
                } catch (...) {
                    errors.push_back(Error::make(ErrorCode::ConfigValidation, "window sizes must be integers (seconds)")
                                         .ctx("location", "windows"));
                }
            }
        }
    }

    // log ---------------------------------------------------------
    if (const YAML::Node l = root["log"]) {
        check_keys(l, {"level", "format", "file", "max_size_mb", "max_files", "quiet_console"}, "log", warnings);
        c.log.level = optional_string(l, "level", c.log.level);
        c.log.format = optional_string(l, "format", c.log.format);
        c.log.file = optional_string(l, "file", "");
        c.log.max_size_mb = optional_int(l, "max_size_mb", c.log.max_size_mb, "log", errors);
        c.log.max_files = static_cast<int>(optional_int(l, "max_files", c.log.max_files, "log", errors));
        c.log.quiet_console = optional_string(l, "quiet_console", "false") == "true";
    }
    {
        std::set<std::string> levels{"trace", "debug", "info", "warn", "error", "critical", "off"};
        if (!levels.count(c.log.level)) {
            errors.push_back(Error::make(ErrorCode::ConfigValidation, "log.level is not valid")
                                 .ctx("location", "log")
                                 .ctx("value", c.log.level));
        }
        if (c.log.format != "text" && c.log.format != "json") {
            errors.push_back(Error::make(ErrorCode::ConfigValidation, "log.format must be 'text' or 'json'")
                                 .ctx("location", "log")
                                 .ctx("value", c.log.format));
        }
        if (c.log.max_size_mb < 1 || c.log.max_files < 1) {
            errors.push_back(
                Error::make(ErrorCode::ConfigValidation, "log.max_size_mb and log.max_files must be at least 1")
                    .ctx("location", "log"));
        }
    }

    // metrics ------------------------------------------------------------
    if (const YAML::Node ms = root["metrics"]) {
        if (!ms.IsSequence() || ms.size() == 0) {
            errors.push_back(
                Error::make(ErrorCode::ConfigValidation, "'metrics' must be a non-empty list").ctx("location", "root"));
        } else {
            std::set<std::string> ids;
            int index = 0;
            for (const auto& item : ms) {
                std::string where = "metrics[" + std::to_string(index) + "]";
                ++index;
                check_keys(item,
                           {"id", "canonical_unit", "input_units", "valid_min", "valid_max", "expected_period",
                            "jitter", "max_gap", "interpolation"},
                           where, warnings);
                MetricCfg m;
                m.id = require_string(item, "id", where, errors);
                m.canonical_unit = require_string(item, "canonical_unit", where, errors);
                if (const YAML::Node units = item["input_units"]) {
                    if (!units.IsSequence() || units.size() == 0) {
                        errors.push_back(
                            Error::make(ErrorCode::ConfigValidation, "input_units must be a non-empty list")
                                .ctx("location", where));
                    } else {
                        for (const auto& u : units) {
                            std::string unit;
                            try {
                                unit = u.as<std::string>();
                            } catch (...) {
                                errors.push_back(
                                    Error::make(ErrorCode::ConfigValidation, "input_units entries must be strings")
                                        .ctx("location", where));
                                continue;
                            }
                            m.input_units.push_back(unit);
                        }
                    }
                } else {
                    errors.push_back(Error::make(ErrorCode::ConfigValidation, "missing required field 'input_units'")
                                         .ctx("location", where));
                }
                m.valid_min = optional_number(item, "valid_min", where, errors);
                m.valid_max = optional_number(item, "valid_max", where, errors);
                m.expected_period_us = optional_duration(item, "expected_period", where, errors);
                m.jitter_us = optional_duration(item, "jitter", where, errors);
                m.max_gap_us = optional_duration(item, "max_gap", where, errors);
                std::string interp = optional_string(item, "interpolation", "none");
                if (interp == "none") {
                    m.interpolation = Interpolation::None;
                } else if (interp == "previous") {
                    m.interpolation = Interpolation::Previous;
                } else if (interp == "linear") {
                    m.interpolation = Interpolation::Linear;
                } else {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, "interpolation must be none|previous|linear")
                            .ctx("location", where)
                            .ctx("value", interp));
                }
                if (valid_id(m.id)) {
                    if (!ids.insert(m.id).second) {
                        errors.push_back(Error::make(ErrorCode::ConfigValidation, "duplicate metric id '" + m.id + "'")
                                             .ctx("location", where));
                    }
                } else {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, kIdChars).ctx("location", where).ctx("value", m.id));
                }
                if (!valid_unit(m.canonical_unit)) {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, "canonical_unit must be 1..32 printable characters")
                            .ctx("location", where)
                            .ctx("value", m.canonical_unit));
                }
                // Unit conversion validation (FR-VAL-004): every unit must be known and all
                // units of one metric must belong to the same dimension.
                if (valid_unit(m.canonical_unit)) {
                    const auto* canonical = core_units::find_unit(m.canonical_unit);
                    if (canonical == nullptr) {
                        errors.push_back(Error::make(ErrorCode::ConfigValidation, "canonical_unit is not a known unit")
                                             .ctx("location", where)
                                             .ctx("value", m.canonical_unit));
                    } else {
                        for (const auto& unit : m.input_units) {
                            const auto* info = core_units::find_unit(unit);
                            if (info == nullptr) {
                                errors.push_back(
                                    Error::make(ErrorCode::ConfigValidation, "input unit is not a known unit")
                                        .ctx("location", where)
                                        .ctx("value", unit));
                            } else if (info->dimension != canonical->dimension) {
                                errors.push_back(Error::make(ErrorCode::ConfigValidation,
                                                             "input unit dimension differs from the canonical unit")
                                                     .ctx("location", where)
                                                     .ctx("value", unit));
                            }
                        }
                    }
                }
                if (m.interpolation != Interpolation::None && !m.expected_period_us) {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, "interpolation requires 'expected_period'")
                            .ctx("location", where));
                }
                if (m.valid_min && m.valid_max && *m.valid_min >= *m.valid_max) {
                    errors.push_back(Error::make(ErrorCode::ConfigValidation, "valid_min must be less than valid_max")
                                         .ctx("location", where));
                }
                c.metrics.push_back(std::move(m));
            }
        }
    } else {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "missing section 'metrics'").ctx("location", "root"));
    }

    // devices --------------------------------------------------------
    if (const YAML::Node ds = root["devices"]) {
        if (!ds.IsSequence() || ds.size() == 0) {
            errors.push_back(
                Error::make(ErrorCode::ConfigValidation, "'devices' must be a non-empty list").ctx("location", "root"));
        } else {
            std::set<std::string> ids;
            int index = 0;
            for (const auto& item : ds) {
                std::string where = "devices[" + std::to_string(index) + "]";
                ++index;
                check_keys(item, {"id", "timezone", "metrics", "tags"}, where, warnings);
                DeviceCfg d;
                d.id = require_string(item, "id", where, errors);
                d.timezone = optional_string(item, "timezone", "UTC");
                if (const YAML::Node midlist = item["metrics"]) {
                    if (midlist.IsSequence()) {
                        for (const auto& mid : midlist) {
                            d.metrics.push_back(mid.as<std::string>());
                        }
                    } else {
                        errors.push_back(Error::make(ErrorCode::ConfigValidation, "'metrics' must be a list")
                                             .ctx("location", where));
                    }
                }
                if (const YAML::Node tags = item["tags"]) {
                    if (!tags.IsMap()) {
                        errors.push_back(Error::make(ErrorCode::ConfigValidation, "'tags' must be a string->string map")
                                             .ctx("location", where));
                    } else {
                        for (const auto& t : tags) {
                            d.tags.emplace_back(t.first.as<std::string>(), t.second.as<std::string>());
                        }
                    }
                }
                if (valid_id(d.id)) {
                    if (!ids.insert(d.id).second) {
                        errors.push_back(Error::make(ErrorCode::ConfigValidation, "duplicate device id '" + d.id + "'")
                                             .ctx("location", where));
                    }
                } else {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, kIdChars).ctx("location", where).ctx("value", d.id));
                }
                if (d.tags.size() > 16) {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, "at most 16 tags per device").ctx("location", where));
                }
                for (const auto& t : d.tags) {
                    if (t.first.empty() || t.first.size() > 128 || t.second.size() > 128) {
                        errors.push_back(
                            Error::make(ErrorCode::ConfigValidation, "tag keys/values must be 1..128 characters")
                                .ctx("location", where));
                    }
                }
                c.devices.push_back(std::move(d));
            }
        }
    } else {
        errors.push_back(Error::make(ErrorCode::ConfigValidation, "missing section 'devices'").ctx("location", "root"));
    }

    // calibrations --------------------------------------------------------
    if (const YAML::Node cs = root["calibrations"]) {
        if (cs.IsSequence()) {
            int index = 0;
            std::set<std::string> calibration_keys;
            for (const auto& item : cs) {
                std::string where = "calibrations[" + std::to_string(index) + "]";
                ++index;
                check_keys(item, {"device", "metric", "reject_unmatched", "segments"}, where, warnings);
                DeviceCalibration dc;
                dc.device_id = require_string(item, "device", where, errors);
                dc.metric_id = require_string(item, "metric", where, errors);
                // Audit #16: duplicate (device, metric) calibration entries would silently
                // shadow each other — reject them like duplicate metric ids.
                std::string calib_key = dc.device_id + "/" + dc.metric_id;
                if (!calib_key.empty() && dc.metric_id != "" && !calibration_keys.insert(calib_key).second) {
                    errors.push_back(Error::make(ErrorCode::ConfigValidation,
                                                 "duplicate calibration entry for device '" + dc.device_id +
                                                     "' and metric '" + dc.metric_id + "'")
                                         .ctx("location", where));
                }
                dc.calibration.reject_unmatched = optional_string(item, "reject_unmatched", "false") == "true";
                if (const YAML::Node segs = item["segments"]) {
                    if (!segs.IsSequence() || segs.size() == 0) {
                        errors.push_back(Error::make(ErrorCode::ConfigValidation, "segments must be a non-empty list")
                                             .ctx("location", where));
                    } else {
                        for (const auto& s : segs) {
                            CalibrationSegment seg;
                            seg.min_inclusive = require_number(s, "min", where, errors);
                            seg.max_exclusive = require_number(s, "max", where, errors);
                            seg.slope = require_number(s, "slope", where, errors);
                            seg.intercept = require_number(s, "intercept", where, errors);
                            dc.calibration.segments.push_back(seg);
                        }
                    }
                } else {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, "missing 'segments'").ctx("location", where));
                }
                c.calibrations.push_back(std::move(dc));
            }
        } else {
            errors.push_back(Error::make(ErrorCode::ConfigValidation, "'calibrations' must be a list (may be empty)")
                                 .ctx("location", "root"));
        }
    }

    // derived_metrics --------------------------------------------------
    if (const YAML::Node xms = root["derived_metrics"]) {
        if (xms.IsSequence()) {
            std::set<std::string> ids;
            int index = 0;
            for (const auto& item : xms) {
                std::string where = "derived_metrics[" + std::to_string(index) + "]";
                ++index;
                check_keys(item, {"id", "unit", "expression", "missing_policy", "max_input_age"}, where, warnings);
                DerivedMetricCfg dm;
                dm.id = require_string(item, "id", where, errors);
                dm.unit = require_string(item, "unit", where, errors);
                dm.expression = require_string(item, "expression", where, errors);
                dm.missing_policy = optional_string(item, "missing_policy", "null");
                dm.max_input_age_us = require_duration(item, "max_input_age", where, errors, 0);
                if (valid_id(dm.id)) {
                    if (!ids.insert(dm.id).second) {
                        errors.push_back(
                            Error::make(ErrorCode::ConfigValidation, "duplicate derived metric id '" + dm.id + "'")
                                .ctx("location", where));
                    }
                } else {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, kIdChars).ctx("location", where).ctx("value", dm.id));
                }
                if (dm.missing_policy != "null" && dm.missing_policy != "recent" && dm.missing_policy != "skip") {
                    errors.push_back(Error::make(ErrorCode::ConfigValidation, "missing_policy must be null|recent|skip")
                                         .ctx("location", where)
                                         .ctx("value", dm.missing_policy));
                }
                c.derived_metrics.push_back(std::move(dm));
            }
        } else {
            errors.push_back(Error::make(ErrorCode::ConfigValidation, "'derived_metrics' must be a list (may be empty)")
                                 .ctx("location", "root"));
        }
    }

    // rules --------------------------------------------------------------
    if (const YAML::Node rs = root["rules"]) {
        if (rs.IsSequence()) {
            std::set<std::string> ids;
            int index = 0;
            for (const auto& item : rs) {
                std::string where = "rules[" + std::to_string(index) + "]";
                ++index;
                check_keys(item,
                           {"id", "type", "severity", "devices", "trigger", "recovery", "duration", "cooldown",
                            "merge_interval"},
                           where, warnings);
                RuleCfg r;
                r.id = require_string(item, "id", where, errors);
                r.type = require_string(item, "type", where, errors);
                r.severity = require_string(item, "severity", where, errors);
                if (valid_id(r.id)) {
                    if (!ids.insert(r.id).second) {
                        errors.push_back(Error::make(ErrorCode::ConfigValidation, "duplicate rule id '" + r.id + "'")
                                             .ctx("location", where));
                    }
                } else {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, kIdChars).ctx("location", where).ctx("value", r.id));
                }
                if (r.type != "threshold" && r.type != "duration" && r.type != "rate" && r.type != "absence" &&
                    r.type != "composite") {
                    errors.push_back(Error::make(ErrorCode::ConfigValidation,
                                                 "rule.type must be threshold|duration|rate|absence|composite")
                                         .ctx("location", where)
                                         .ctx("value", r.type));
                }
                if (r.severity != "info" && r.severity != "warning" && r.severity != "high" &&
                    r.severity != "critical") {
                    errors.push_back(
                        Error::make(ErrorCode::ConfigValidation, "rule.severity must be info|warning|high|critical")
                            .ctx("location", where)
                            .ctx("value", r.severity));
                }
                // M3 fields: trigger expression, recovery expression, timing, device selector.
                r.trigger = require_string(item, "trigger", where, errors);
                r.recovery = optional_string(item, "recovery", "");
                r.duration_us = require_duration(item, "duration", where, errors, 0);
                r.cooldown_us = require_duration(item, "cooldown", where, errors, 0);
                r.merge_interval_us = require_duration(item, "merge_interval", where, errors, 0);
                if (const YAML::Node dev_sel = item["devices"]) {
                    if (dev_sel.IsMap()) {
                        for (const auto& t : dev_sel) {
                            r.devices.emplace_back(t.first.as<std::string>(), t.second.as<std::string>());
                        }
                    } else {
                        errors.push_back(
                            Error::make(ErrorCode::ConfigValidation, "'devices' must be a string->string map")
                                .ctx("location", where));
                    }
                }
                c.rules.push_back(std::move(r));
            }
        } else {
            errors.push_back(Error::make(ErrorCode::ConfigValidation, "'rules' must be a list (may be empty)")
                                 .ctx("location", "root"));
        }
    }

    // retention ---------------------------------------------------------
    if (const YAML::Node rt = root["retention"]) {
        check_keys(rt, {"samples_days", "aggregates_days", "incidents_days", "audit_days"}, "retention", warnings);
        c.retention.samples_days = optional_int(rt, "samples_days", c.retention.samples_days, "retention", errors);
        c.retention.aggregates_days =
            optional_int(rt, "aggregates_days", c.retention.aggregates_days, "retention", errors);
        c.retention.incidents_days =
            optional_int(rt, "incidents_days", c.retention.incidents_days, "retention", errors);
        c.retention.audit_days = optional_int(rt, "audit_days", c.retention.audit_days, "retention", errors);
    }
    if (c.retention.samples_days < 1 || c.retention.aggregates_days < 1 || c.retention.incidents_days < 1 ||
        c.retention.audit_days < 1) {
        errors.push_back(
            Error::make(ErrorCode::ConfigValidation, "retention days must be at least 1").ctx("location", "retention"));
    }

    // Cross-references ------------------------------------------------
    for (const auto& d : c.devices) {
        for (const auto& mid : d.metrics) {
            if (!c.metric(mid)) {
                errors.push_back(Error::make(ErrorCode::ConfigValidation,
                                             "device '" + d.id + "' references unknown metric '" + mid + "'")
                                     .ctx("location", "devices"));
            }
        }
    }
    for (const auto& cal : c.calibrations) {
        const DeviceCfg* dev = c.device(cal.device_id);
        if (!dev) {
            errors.push_back(Error::make(ErrorCode::ConfigValidation,
                                         "calibration references unknown device '" + cal.device_id + "'")
                                 .ctx("location", "calibrations"));
        } else if (!c.metric(cal.metric_id)) {
            errors.push_back(Error::make(ErrorCode::ConfigValidation,
                                         "calibration references unknown metric '" + cal.metric_id + "'")
                                 .ctx("location", "calibrations"));
        } else if (!cfg->metric_allowed_for_device(*dev, cal.metric_id)) {
            errors.push_back(
                Error::make(ErrorCode::ConfigValidation,
                            "metric '" + cal.metric_id + "' is not allowed for device '" + cal.device_id + "'")
                    .ctx("location", "calibrations"));
        }
        auto segs = cal.calibration.segments;
        std::sort(segs.begin(), segs.end(), [](const CalibrationSegment& a, const CalibrationSegment& b) {
            return a.min_inclusive < b.min_inclusive;
        });
        for (size_t i = 0; i < segs.size(); ++i) {
            if (!(segs[i].min_inclusive < segs[i].max_exclusive)) {
                errors.push_back(Error::make(ErrorCode::ConfigValidation, "calibration segment must satisfy min < max")
                                     .ctx("location", "calibrations[" + std::to_string(i) + "]"));
            }
            if (i + 1 < segs.size() && segs[i].max_exclusive > segs[i + 1].min_inclusive) {
                errors.push_back(Error::make(ErrorCode::ConfigValidation, "calibration segments must not overlap")
                                     .ctx("location", "calibrations"));
            }
        }
    }
    for (const auto& dm : c.derived_metrics) {
        if (c.metric(dm.id)) {
            errors.push_back(Error::make(ErrorCode::ConfigValidation,
                                         "derived metric id '" + dm.id + "' collides with a base metric id")
                                 .ctx("location", "derived_metrics"));
        }
    }

    if (!errors.empty()) {
        // Report the first few problems; context carries the location.
        Error combined = errors[0];
        for (size_t i = 1; i < errors.size() && i < 8; ++i) {
            combined.ctx("also", errors[i].message + " [" +
                                     (errors[i].context.empty() ? "" : errors[i].context.front().second) + "]");
        }
        if (errors.size() > 8)
            combined.ctx("also", "… and more");
        return Result<std::shared_ptr<const ConfigSnapshot>>::Err(std::move(combined));
    }

    // Resolve timezones ---------------------------------------------------
    cfg->default_timezone = locate_timezone("UTC");
    for (const auto& d : c.devices) {
        const date::time_zone* zone = locate_timezone(d.timezone);
        if (zone == nullptr) {
            return Result<std::shared_ptr<const ConfigSnapshot>>::Err(
                Error::make(ErrorCode::ConfigValidation, "unknown IANA timezone '" + d.timezone + "'")
                    .ctx("location", "devices")
                    .ctx("device", d.id));
        }
        cfg->device_timezones[d.id] = zone;
    }

    // Resolve relative paths against the config file directory ------------
    auto resolve = [&base_dir](std::string& p) {
        if (!p.empty() && p[0] != '/') {
            p = (base_dir.empty() ? "." : base_dir) + "/" + p;
        }
    };
    resolve(c.directories.input);
    resolve(c.directories.archive);
    resolve(c.directories.quarantine);
    resolve(c.directories.reports);
    resolve(c.database.path);
    resolve(c.log.file);

    cfg->version = compute_config_version(c);
    return Result<std::shared_ptr<const ConfigSnapshot>>::Ok(std::move(cfg));
}

} // namespace

Result<std::shared_ptr<const ConfigSnapshot>> load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return Result<std::shared_ptr<const ConfigSnapshot>>::Err(
            Error::make(ErrorCode::ConfigFileOpen, "cannot open configuration file").ctx("path", path));
    }
    std::string base_dir;
    {
        size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            base_dir = ".";
        } else if (slash == 0) {
            base_dir = "/";
        } else {
            base_dir = path.substr(0, slash);
        }
    }
    YAML::Node root;
    try {
        root = YAML::Load(in);
    } catch (const YAML::Exception& ex) {
        return Result<std::shared_ptr<const ConfigSnapshot>>::Err(
            Error::make(ErrorCode::ConfigParse, std::string("YAML parse error: ") + ex.what()).ctx("path", path));
    }
    return parse_and_validate(root, base_dir);
}

} // namespace streamforge
