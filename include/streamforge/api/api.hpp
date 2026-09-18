#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"
#include "streamforge/rules/rule_engine.hpp"
#include "streamforge/storage/store.hpp"

namespace streamforge {
namespace api {

// Resolves the admin bearer token (FR-API-004): the STREAMFORGE_ADMIN_TOKEN environment
// variable takes precedence, then the configured token file (which must not be readable
// by group/others). An empty result means admin endpoints refuse every request.
Result<std::string> resolve_admin_token(const std::string& token_file);

// Constant-time comparison used for bearer tokens.
bool constant_time_equals(const std::string& a, const std::string& b);

// Asynchronous import/report job tracked by the host.
struct JobInfo {
    enum class State { Queued, Running, Completed, Failed };
    std::string id;
    State state = State::Queued;
    std::string path;
    std::string error;
    int64_t accepted = 0;
    int64_t total = 0;
    std::optional<int64_t> finished_at_us;
};

// Bridge to host-side operations. Keeps the HTTP layer free of pipeline dependencies
// (requirement 10.4: layers communicate through explicit interfaces).
class ApiServices {
public:
    virtual ~ApiServices() = default;
    [[nodiscard]] virtual std::shared_ptr<const ConfigSnapshot> current_config() const = 0;
    [[nodiscard]] virtual bool ready() const = 0;
    [[nodiscard]] virtual Result<void> reload_config(const std::string& request_id, const std::string& actor) = 0;
    [[nodiscard]] virtual Result<std::string> submit_import(const std::string& path, bool recursive) = 0;
    [[nodiscard]] virtual JobInfo job(const std::string& id) const = 0;
};

struct HttpConfig {
    std::string bind = "127.0.0.1";
    int port = 8080;
};

// The M3/M4 HTTP service (FR-API-001..004): health/ready, catalog and time-series
// queries, incident lifecycle, admin operations and the Prometheus /metrics endpoint.
// The concrete HTTP implementation is hidden behind this facade.
class HttpApi {
public:
    HttpApi(storage::Store& store, rules::RuleEngine& rules, ApiServices& services);
    ~HttpApi();

    // Binds and starts serving on an internal thread. An empty admin token disables
    // every admin endpoint (they answer 503 until a token is configured).
    Result<void> start(const HttpConfig& cfg, std::string admin_token);
    // Stops serving and joins the thread.
    void stop();
    // Actual port in use (differs from the config when 0/ephemeral; tests).
    [[nodiscard]] int bound_port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace api
} // namespace streamforge
