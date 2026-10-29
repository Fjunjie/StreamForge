#pragma once

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "streamforge/api/api.hpp"
#include "streamforge/ingest/pipeline.hpp"
#include "streamforge/rules/rule_engine.hpp"
#include "streamforge/storage/store.hpp"

namespace streamforge {
namespace api {

// Host-side glue between the HTTP API and the running services (requirement
// FR-CFG-003): owns the active configuration snapshot, implements config hot reload
// (validate first, reject restart-only changes, atomic swap) and tracks admin import
// jobs executed by the server main loop. All methods are thread-safe.
class AppRuntime : public ApiServices {
public:
    AppRuntime(storage::Store& store, rules::RuleEngine& rules, ImportPipeline& pipeline);

    // Startup path: installs the configuration without restart-restriction checks.
    Result<void> start_with(std::shared_ptr<const ConfigSnapshot> cs, const std::string& config_path);

    // Hot reload path: loads the config file, validates, keeps the previous snapshot
    // on any failure. Writes an audit record for both outcomes (FR-OBS-003).
    Result<void> reload_config(const std::string& request_id, const std::string& actor) override;

    // ApiServices
    [[nodiscard]] std::shared_ptr<const ConfigSnapshot> current_config() const override;
    [[nodiscard]] bool ready() const override;
    Result<std::string> submit_import(const std::string& path, bool recursive) override;
    [[nodiscard]] JobInfo job(const std::string& id) const override;

    // Pending import jobs for the main loop; returns and clears the queue.
    struct PendingImport {
        std::string job_id;
        std::string path;
    };
    [[nodiscard]] std::vector<PendingImport> take_pending_imports();
    // Main loop reports a finished import; `accepted`/`total` surface in job status.
    void complete_job(const std::string& job_id, bool ok, const std::string& error, int64_t accepted, int64_t total);
    void start_job(const std::string& job_id);

private:
    Result<void> apply(std::shared_ptr<const ConfigSnapshot> cs, const std::string& request_id,
                       const std::string& actor, bool startup);

    storage::Store& store_;
    rules::RuleEngine& rules_;
    ImportPipeline& pipeline_;
    std::string config_path_;
    mutable std::mutex mu_;
    std::shared_ptr<const ConfigSnapshot> config_;
    bool ready_ = false;
    std::map<std::string, JobInfo> jobs_;
    std::deque<PendingImport> pending_;
    uint64_t job_seq_ = 0;
};

} // namespace api
} // namespace streamforge
