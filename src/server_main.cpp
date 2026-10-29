// streamforge-server: directory watching, data import, derived metrics, rule evaluation,
// the HTTP API, config hot reload and data retention.
//
// Shutdown model (FR-REC-003): SIGTERM/SIGINT stop intake and the HTTP service, the
// in-flight batch finishes (or hits the stop flag between batches) and the process exits
// after joining every worker thread. SIGHUP triggers a config hot reload (FR-CFG-003).
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "streamforge/api/api.hpp"
#include "streamforge/api/runtime.hpp"
#include "streamforge/config/config.hpp"
#include "streamforge/core/log.hpp"
#include "streamforge/core/metrics.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/ingest/discovery.hpp"
#include "streamforge/ingest/pipeline.hpp"
#include "streamforge/processing/sample_buffer.hpp"
#include "streamforge/rules/rule_engine.hpp"
#include "streamforge/storage/store.hpp"

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_reload{false};

extern "C" void handle_signal(int sig) {
    if (sig == SIGHUP) {
        g_reload.store(true);
        return;
    }
    g_stop.store(true);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr); // config hot reload (FR-CFG-003)
}

void sleep_interruptible(std::chrono::milliseconds total, const std::atomic<bool>& stop) {
    constexpr std::chrono::milliseconds kSlice{100};
    auto remaining = total;
    while (remaining > std::chrono::milliseconds::zero() && !stop.load()) {
        auto step = std::min(remaining, kSlice);
        std::this_thread::sleep_for(step);
        remaining -= step;
    }
}

} // namespace

int run_server_main(int argc, char** argv) {
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: streamforge-server --config <file>\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            std::fprintf(stderr, "usage: streamforge-server --config <file>\n");
            return 2;
        }
    }
    if (config_path.empty()) {
        std::fprintf(stderr, "usage: streamforge-server --config <file>\n");
        return 2;
    }

    // Console logging first so startup failures are visible.
    streamforge::LogConfig early_log;
    early_log.level = "info";
    streamforge::init_logging(early_log);

    auto loaded = streamforge::load_config(config_path);
    if (!loaded.ok()) {
        std::fprintf(stderr, "streamforge-server: invalid configuration: %s (code %s)\n",
                     loaded.error().message.c_str(), loaded.error().code_name());
        for (const auto& [k, v] : loaded.error().context) {
            std::fprintf(stderr, "  %s: %s\n", k.c_str(), v.c_str());
        }
        return 1;
    }
    auto cs = loaded.take();

    streamforge::init_logging(cs->cfg.log);
    auto log = streamforge::logger("server");
    log->info("configuration loaded (config_version={}, base={})", cs->version, config_path);

    auto store_rc = streamforge::storage::Store::open(cs->cfg.database.path);
    if (!store_rc.ok()) {
        log->critical("cannot open database {}: {}", cs->cfg.database.path, store_rc.error().message);
        return 1;
    }
    auto store = store_rc.take();

    auto synced = store->sync_catalog(*cs);
    if (!synced.ok()) {
        log->critical("catalog sync failed: {}", synced.error().message);
        return 1;
    }

    // Input directory must exist; the other directories are created on demand.
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(cs->cfg.directories.input), ec)) {
        log->critical("input directory does not exist: {}", cs->cfg.directories.input);
        return 1;
    }

    streamforge::ImportPipeline pipeline(cs, store);
    pipeline.should_stop = [] { return g_stop.load(); };

    streamforge::rules::RuleEngine rules(*store);
    auto rules_loaded = rules.load_rules(*cs);
    if (!rules_loaded.ok()) {
        log->critical("rule compilation failed: {}", rules_loaded.error().message);
        return 1;
    }

    // Rolling window state for rule AVG/DELTA/RATE (FR-AGG-002): a fixed generous
    // window (24h, count-capped per series) so reloads never invalidate the provider.
    streamforge::processing::SampleBuffer buffers(24LL * 3600 * 1000000, 8192);
    rules.set_window_provider(
        [&buffers](const std::string& device, const std::string& metric, int64_t duration_us,
                   const std::string& func) { return buffers.window(device, metric, duration_us, func); });

    // Durable samples feed the rolling state and the rule engine, batch by batch.
    pipeline.on_samples_committed = [&](const std::vector<streamforge::ImportPipeline::CommittedSample>& batch) {
        std::set<std::string> devices;
        int64_t newest = 0;
        for (const auto& c : batch) {
            buffers.add(c.device_id, c.metric_id, c.event_time_us, c.value);
            devices.insert(c.device_id);
            newest = std::max(newest, c.event_time_us);
        }
        auto& metrics = streamforge::MetricsRegistry::instance();
        metrics.inc_counter("sf_samples_total", {{"kind", "committed"}}, static_cast<double>(batch.size()));
        for (const auto& device : devices) {
            std::map<std::string, double> metric_values;
            for (const auto& [metric, latest] : buffers.latest_by_metric(device))
                metric_values[metric] = latest.value;
            auto evaluated = rules.evaluate_device(device, newest, metric_values, {});
            if (!evaluated.ok())
                log->warn("rule evaluation failed for device {}: {}", device, evaluated.error().message);
        }
        auto open = store->count_incidents("open");
        if (open.ok())
            metrics.set_gauge("sf_incidents_open", {}, static_cast<double>(open.value()));
    };

    streamforge::api::AppRuntime runtime(*store, rules, pipeline);
    if (!runtime.start_with(cs, config_path).ok()) {
        log->critical("runtime initialization failed");
        return 1;
    }

    streamforge::api::HttpConfig http_cfg;
    http_cfg.bind = cs->cfg.server.bind;
    http_cfg.port = cs->cfg.server.port;
    auto token = streamforge::api::resolve_admin_token(cs->cfg.server.admin_token_file);
    if (!token.ok()) {
        log->critical("admin token unavailable: {}", token.error().message);
        return 1;
    }
    if (token.value().empty())
        log->warn("no admin token configured: admin endpoints are disabled");
    streamforge::api::HttpApi http(*store, rules, runtime);
    auto http_started = http.start(http_cfg, token.value());
    if (!http_started.ok()) {
        log->critical("http service failed to start on {}:{}", cs->cfg.server.bind, cs->cfg.server.port);
        return 1;
    }
    log->info("http api listening on {}:{}", cs->cfg.server.bind, http.bound_port());

    // Data retention worker (FR-DB-004): startup pass, then once per hour, batched.
    std::atomic<bool> retention_stop{false};
    std::thread retention_worker([&] {
        while (!retention_stop.load()) {
            auto snapshot = runtime.current_config();
            if (snapshot) {
                auto purged = store->purge_retention(snapshot->cfg.retention, 10000);
                if (purged.ok() && purged.value() > 0) {
                    log->info("retention purge removed {} rows", purged.value());
                    store->audit("retention_purge", "system", "db", "ok", "",
                                 nlohmann::json{{"deleted", purged.value()}}.dump());
                } else if (!purged.ok()) {
                    log->error("retention purge failed: {}", purged.error().message);
                }
            }
            for (int i = 0; i < 360 && !retention_stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    });

    auto recovered = pipeline.recover_pending();
    if (recovered.ok()) {
        const auto& s = recovered.value();
        log->info("recovery pass: resumed={} source_changed={} archived={} missing={}", s.resumed, s.source_changed,
                  s.archived, s.missing);
    } else {
        log->error("recovery pass failed: {}", recovered.error().message);
    }

    install_signal_handlers();
    log->info("watching input directory {} (scan interval {}ms)", cs->cfg.directories.input,
              cs->cfg.pipeline.scan_interval_ms);

    streamforge::DirectoryScanner scanner(cs->cfg.directories.input,
                                          std::chrono::milliseconds(cs->cfg.pipeline.quiet_period_ms));
    auto& metrics = streamforge::MetricsRegistry::instance();

    while (!g_stop.load()) {
        if (g_reload.exchange(false)) {
            log->info("SIGHUP received: reloading configuration");
            auto reloaded = runtime.reload_config("sighup", "sighup");
            if (!reloaded.ok())
                log->error("config reload failed (previous configuration stays active): {}", reloaded.error().message);
        }
        for (const auto& job : runtime.take_pending_imports()) {
            if (g_stop.load())
                break;
            runtime.start_job(job.job_id);
            auto result = pipeline.process_file(job.path);
            runtime.complete_job(job.job_id, result.outcome == streamforge::ImportPipeline::ProcessResult::Outcome::Completed,
                                 result.error.message, result.accepted, result.total);
        }
        auto hits = scanner.scan();
        if (!hits.ok()) {
            log->error("directory scan failed: {}", hits.error().message);
            sleep_interruptible(std::chrono::milliseconds(cs->cfg.pipeline.scan_interval_ms), g_stop);
            continue;
        }
        for (const auto& hit : hits.value()) {
            if (g_stop.load())
                break; // stop accepting new files (FR-REC-003)
            auto result = pipeline.process_file(hit.path);
            switch (result.outcome) {
            case streamforge::ImportPipeline::ProcessResult::Outcome::Completed:
                log->info("processed {}: accepted={} format_errors={} business_errors={}", hit.path, result.accepted,
                          result.format_errors, result.business_errors);
                break;
            case streamforge::ImportPipeline::ProcessResult::Outcome::Quarantined:
                log->warn("quarantined {}: business_errors={} total={}", hit.path, result.business_errors,
                          result.total);
                break;
            case streamforge::ImportPipeline::ProcessResult::Outcome::SkippedDuplicate:
                log->info("skipped duplicate identity: {}", hit.path);
                break;
            case streamforge::ImportPipeline::ProcessResult::Outcome::Interrupted:
                log->info("processing paused for shutdown: {}", hit.path);
                break;
            case streamforge::ImportPipeline::ProcessResult::Outcome::Failed:
                log->error("failed {}: {} (code {})", hit.path, result.error.message, result.error.code_name());
                break;
            }
        }
        sleep_interruptible(std::chrono::milliseconds(cs->cfg.pipeline.scan_interval_ms), g_stop);
    }

    log->info("shutting down: stopping http service");
    http.stop();
    retention_stop.store(true);
    retention_worker.join();
    log->info("shutdown complete");
    streamforge::shutdown_logging();
    return 0;
}

int main(int argc, char** argv) {
    try {
        return run_server_main(argc, argv);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "streamforge-server: fatal: %s\n", ex.what());
        return 1;
    }
}
