// streamforge-server: directory watching, data import and (from M3) the HTTP service.
// M1 scope: load configuration, prepare the database, recover interrupted files, then poll
// the input directory and run the import pipeline with graceful shutdown on SIGTERM/SIGINT.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>

#include <spdlog/spdlog.h>

#include "streamforge/config/config.hpp"
#include "streamforge/core/log.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/ingest/discovery.hpp"
#include "streamforge/ingest/pipeline.hpp"
#include "streamforge/storage/store.hpp"

namespace {

std::atomic<bool> g_stop{false};

extern "C" void handle_signal(int) {
    g_stop.store(true);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // SIGHUP (config hot reload) is delivered in M3; ignore it for now so a stray
    // signal does not kill the process.
    signal(SIGHUP, SIG_IGN);
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

    while (!g_stop.load()) {
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
