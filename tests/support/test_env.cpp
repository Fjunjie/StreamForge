#include "support/test_env.hpp"

#include <unistd.h>

#include <chrono>
#include <fstream>

#include "streamforge/core/time.hpp"

namespace sf_test {

TempDir::TempDir() {
    auto base = fs::temp_directory_path();
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = (base / ("sf_test_" + std::to_string(stamp) + "_" + std::to_string(::getpid()))).string();
    fs::create_directories(path);
}

TempDir::~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
}

std::string TempDir::sub(const std::string& name) const {
    std::error_code ec;
    fs::create_directories(fs::path(path) / name, ec);
    return (fs::path(path) / name).string();
}

std::string TempDir::file(const std::string& name, const std::string& content) const {
    fs::path full = fs::path(path) / name;
    std::error_code ec;
    if (full.has_parent_path()) {
        fs::create_directories(full.parent_path(), ec);
    }
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    out << content;
    return full.string();
}

std::shared_ptr<const streamforge::ConfigSnapshot> make_config(const std::string& root, int64_t batch_size) {
    using namespace streamforge;
    auto snapshot = std::make_shared<ConfigSnapshot>();
    Config& c = snapshot->cfg;

    c.base_dir = root;
    c.directories.input = (fs::path(root) / "input").string();
    c.directories.archive = (fs::path(root) / "archive").string();
    c.directories.quarantine = (fs::path(root) / "quarantine").string();
    c.directories.reports = (fs::path(root) / "reports").string();
    c.database.path = (fs::path(root) / "test.db").string();
    c.pipeline.batch_size = batch_size;
    c.pipeline.max_error_rate = 0.10;
    c.pipeline.error_rate_min_records = 20;
    c.pipeline.history_range_us = 365LL * 24 * 3600 * 1000000; // generous for tests
    c.pipeline.allowed_lateness_us = 300LL * 1000000;
    c.pipeline.quiet_period_ms = 0;
    c.pipeline.scan_interval_ms = 200;

    MetricCfg temp;
    temp.id = "temp";
    temp.canonical_unit = "C";
    temp.input_units = {"C", "F", "K"};
    MetricCfg pressure;
    pressure.id = "pressure";
    pressure.canonical_unit = "kPa";
    pressure.input_units = {"Pa", "kPa", "MPa", "bar"};
    MetricCfg flow;
    flow.id = "flow";
    flow.canonical_unit = "m3/h";
    flow.input_units = {"L/min", "m3/h", "m3/s"};
    c.metrics = {temp, pressure, flow};

    DeviceCfg dev1;
    dev1.id = "dev-01";
    dev1.timezone = "UTC";
    DeviceCfg dev2;
    dev2.id = "dev-02";
    dev2.timezone = "Asia/Shanghai";
    dev2.tags = {{"line", "A"}};
    c.devices = {dev1, dev2};

    snapshot->default_timezone = locate_timezone("UTC");
    for (const auto& d : c.devices) {
        snapshot->device_timezones[d.id] = locate_timezone(d.timezone);
    }
    snapshot->version = compute_config_version(c);
    return snapshot;
}

} // namespace sf_test
