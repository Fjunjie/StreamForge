#include "streamforge/api/runtime.hpp"

#include <filesystem>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "streamforge/core/log.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/core/uuid.hpp"

namespace {
using json = nlohmann::json;
} // namespace

namespace streamforge {
namespace api {

namespace fs = std::filesystem;

AppRuntime::AppRuntime(storage::Store& store, rules::RuleEngine& rules, ImportPipeline& pipeline)
    : store_(store), rules_(rules), pipeline_(pipeline) {}

Result<void> AppRuntime::start_with(std::shared_ptr<const ConfigSnapshot> cs, const std::string& config_path) {
    config_path_ = config_path;
    std::lock_guard<std::mutex> lk(mu_);
    config_ = std::move(cs);
    ready_ = true;
    return Result<void>::Ok();
}

// Hot reload (FR-CFG-003): validate completely, refuse restart-only changes, then swap
// atomically. Any failure keeps the previous configuration fully active.
Result<void> AppRuntime::reload_config(const std::string& request_id, const std::string& actor) {
    std::shared_ptr<const ConfigSnapshot> previous;
    {
        std::lock_guard<std::mutex> lk(mu_);
        previous = config_;
    }
    if (!previous || config_path_.empty()) {
        return Result<void>::Err(Error::make(ErrorCode::ConfigValidation, "no configuration loaded to reload"));
    }
    auto loaded = load_config(config_path_);
    if (!loaded.ok()) {
        auto audited = store_.audit("config_reload", actor, config_path_, "failed", request_id,
                                    json{{"error", loaded.error().message}}.dump());
        (void)audited;
        return Result<void>::Err(loaded.error());
    }
    auto applied = apply(loaded.value(), request_id, actor, /*startup=*/false);
    if (!applied.ok()) {
        auto audited = store_.audit("config_reload", actor, config_path_, "failed", request_id,
                                    json{{"error", applied.error().message}}.dump());
        (void)audited;
        return applied;
    }
    auto audited = store_.audit("config_reload", actor, config_path_, "ok", request_id,
                                json{{"config_version", static_cast<int64_t>(loaded.value()->version)}}.dump());
    (void)audited;
    SPDLOG_LOGGER_INFO(logger("server"), "configuration reloaded (version {})", loaded.value()->version);
    return Result<void>::Ok();
}

Result<void> AppRuntime::apply(std::shared_ptr<const ConfigSnapshot> cs, const std::string& request_id,
                               const std::string& actor, bool startup) {
    std::shared_ptr<const ConfigSnapshot> previous;
    {
        std::lock_guard<std::mutex> lk(mu_);
        previous = config_;
    }
    if (!startup && previous) {
        // Restart-only fields (FR-CFG-003): database path, bind address, port, thread
        // count and queue capacity must not change on a running system.
        if (previous->cfg.database.path != cs->cfg.database.path) {
            return Result<void>::Err(Error::make(ErrorCode::ConfigValidation,
                                                 "database.path change requires a restart")
                                         .ctx("field", "database.path"));
        }
        if (previous->cfg.server.bind != cs->cfg.server.bind || previous->cfg.server.port != cs->cfg.server.port) {
            return Result<void>::Err(
                Error::make(ErrorCode::ConfigValidation, "server bind/port change requires a restart")
                    .ctx("field", "server"));
        }
        if (previous->cfg.pipeline.workers != cs->cfg.pipeline.workers) {
            return Result<void>::Err(Error::make(ErrorCode::ConfigValidation,
                                                 "pipeline.workers change requires a restart")
                                         .ctx("field", "pipeline.workers"));
        }
        if (previous->cfg.pipeline.queue_capacity != cs->cfg.pipeline.queue_capacity) {
            return Result<void>::Err(Error::make(ErrorCode::ConfigValidation,
                                                 "pipeline.queue_capacity change requires a restart")
                                         .ctx("field", "pipeline.queue_capacity"));
        }
    }

    // Apply to the subsystems in dependency order; each step validates before the next.
    auto catalog = store_.sync_catalog(*cs);
    if (!catalog.ok())
        return catalog;
    auto derived = pipeline_.update_config(cs);
    if (!derived.ok())
        return derived;
    auto rules = rules_.load_rules(*cs);
    if (!rules.ok())
        return rules;

    {
        std::lock_guard<std::mutex> lk(mu_);
        config_ = std::move(cs);
        ready_ = true;
    }
    return Result<void>::Ok();
}

std::shared_ptr<const ConfigSnapshot> AppRuntime::current_config() const {
    std::lock_guard<std::mutex> lk(mu_);
    return config_;
}

bool AppRuntime::ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ready_;
}

Result<std::string> AppRuntime::submit_import(const std::string& path, bool recursive) {
    (void)recursive; // recursion handled by the main loop's file discovery on directories
    auto cs = current_config();
    if (!cs)
        return Result<std::string>::Err(Error::make(ErrorCode::ConfigValidation, "no configuration loaded"));

    // Path normalization and containment (requirement 10.3): the submitted path must be
    // an existing regular file inside the configured input directory.
    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(fs::path(path), ec);
    if (ec)
        return Result<std::string>::Err(Error::make(ErrorCode::PathInvalid, "import path could not be normalized")
                                            .ctx("path", path));
    fs::path input_root = fs::weakly_canonical(fs::path(cs->cfg.directories.input), ec);
    if (ec)
        return Result<std::string>::Err(Error::make(ErrorCode::PathInvalid, "input directory is not resolvable"));
    if (normalized.string().rfind(input_root.string(), 0) != 0) {
        return Result<std::string>::Err(
            Error::make(ErrorCode::PathOutsideRoot, "import path is outside the input directory").ctx("path", path));
    }
    if (!fs::is_regular_file(normalized, ec) || ec) {
        return Result<std::string>::Err(Error::make(ErrorCode::PathInvalid, "import path is not a regular file")
                                            .ctx("path", path));
    }

    std::lock_guard<std::mutex> lk(mu_);
    std::string job_id = "job-" + std::to_string(++job_seq_) + "-" + uuid_v4().substr(0, 8);
    JobInfo info;
    info.id = job_id;
    info.state = JobInfo::State::Queued;
    info.path = normalized.string();
    jobs_[job_id] = std::move(info);
    pending_.push_back(PendingImport{job_id, normalized.string()});
    return Result<std::string>::Ok(job_id);
}

std::vector<AppRuntime::PendingImport> AppRuntime::take_pending_imports() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PendingImport> out(std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
    pending_.clear();
    return out;
}

void AppRuntime::start_job(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(job_id);
    if (it != jobs_.end())
        it->second.state = JobInfo::State::Running;
}

void AppRuntime::complete_job(const std::string& job_id, bool ok, const std::string& error, int64_t accepted,
                              int64_t total) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end())
        return;
    it->second.state = ok ? JobInfo::State::Completed : JobInfo::State::Failed;
    it->second.error = error;
    it->second.accepted = accepted;
    it->second.total = total;
    it->second.finished_at_us = now_unix_us();
    auto audited = store_.audit("admin_import", "api", it->second.path, ok ? "ok" : "failed", "", 
                                json{{"job_id", job_id}, {"error", error}}.dump());
    (void)audited;
}

JobInfo AppRuntime::job(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    return it == jobs_.end() ? JobInfo{} : it->second;
}

} // namespace api
} // namespace streamforge
