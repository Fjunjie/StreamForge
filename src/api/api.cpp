#include "streamforge/api/api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "streamforge/core/log.hpp"
#include "streamforge/core/metrics.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/core/uuid.hpp"

namespace streamforge {
namespace api {

namespace {

using json = nlohmann::json;
constexpr size_t kMaxBodyBytes = 1024 * 1024; // FR-SEC: 1 MiB default request limit

struct RequestContext {
    std::string request_id;
};

std::string iso8601_us(int64_t us) {
    return format_utc_us(TimePointUs{std::chrono::duration_cast<TimePointUs::duration>(std::chrono::microseconds(us))});
}

json error_body(ErrorCode code, const std::string& message, const std::string& request_id) {
    json body;
    body["code"] = Error::make(code, message).code_name();
    body["message"] = message;
    body["request_id"] = request_id;
    return body;
}

// Common parameter extraction: returns nullopt (and writes the response) on error.
std::optional<int64_t> time_param(const httplib::Request& req, const char* name, bool required,
                                  const RequestContext& rc, httplib::Response& res) {
    if (!req.has_param(name)) {
        if (required) {
            res.status = 400;
            res.body = error_body(ErrorCode::InvalidArgument, std::string("missing required parameter '") + name + "'",
                                  rc.request_id)
                           .dump();
        }
        return std::nullopt;
    }
    auto parsed = parse_admin_time(req.get_param_value(name));
    if (!parsed.ok()) {
        res.status = 400;
        res.body = error_body(ErrorCode::InvalidArgument, "invalid time parameter '" + std::string(name) + "': " +
                                                                parsed.error().message,
                              rc.request_id)
                       .dump();
        return std::nullopt;
    }
    return std::optional<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(parsed.value().time_since_epoch()).count());
}

std::optional<int64_t> int_param(const httplib::Request& req, const char* name, int64_t fallback,
                                 const RequestContext& rc, httplib::Response& res) {
    if (!req.has_param(name))
        return fallback;
    try {
        size_t consumed = 0;
        int64_t v = std::stoll(req.get_param_value(name), &consumed);
        if (consumed != req.get_param_value(name).size()) {
            res.status = 400;
            res.body = error_body(ErrorCode::InvalidArgument, "invalid integer parameter '" + std::string(name) + "'",
                                  rc.request_id)
                           .dump();
            return std::nullopt;
        }
        return v;
    } catch (const std::exception&) {
        res.status = 400;
        res.body = error_body(ErrorCode::InvalidArgument, "invalid integer parameter '" + std::string(name) + "'",
                              rc.request_id)
                       .dump();
        return std::nullopt;
    }
}

json incident_json(const storage::IncidentRow& r) {
    json j;
    j["id"] = r.uuid;
    j["rule_id"] = r.rule_id;
    j["rule_version"] = r.rule_version;
    j["device_id"] = r.device_id;
    j["severity"] = r.severity;
    j["state"] = r.state;
    j["started_at"] = iso8601_us(r.started_us);
    if (r.ended_us)
        j["ended_at"] = iso8601_us(*r.ended_us);
    j["hit_count"] = r.hit_count;
    j["reopen_count"] = r.reopen_count;
    if (!r.acked_by.empty()) {
        j["ack"] = {{"by", r.acked_by}, {"comment", r.ack_comment}};
    }
    return j;
}

} // namespace

Result<std::string> resolve_admin_token(const std::string& token_file) {
    const char* env = std::getenv("STREAMFORGE_ADMIN_TOKEN");
    if (env != nullptr && env[0] != '\0')
        return Result<std::string>::Ok(std::string(env));
    if (token_file.empty())
        return Result<std::string>::Ok(std::string());
    std::ifstream in(token_file);
    if (!in) {
        return Result<std::string>::Err(
            Error::make(ErrorCode::IoOpen, "admin token file could not be opened").ctx("file", token_file));
    }
    std::string token;
    std::getline(in, token);
    while (!token.empty() && (token.back() == '\r' || token.back() == '\n'))
        token.pop_back();
    // The token file must not be readable by group or others (FR-API-004).
    return Result<std::string>::Ok(token);
}

bool constant_time_equals(const std::string& a, const std::string& b) {
    // Diff accumulator walks the full length regardless of where a mismatch occurs.
    const size_t n = a.size() < b.size() ? b.size() : a.size();
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

struct HttpApi::Impl {
    storage::Store& store;
    rules::RuleEngine& rules;
    ApiServices& services;
    std::string admin_token;
    httplib::Server svr;
    std::thread worker;
    int bound_port = 0;

    Impl(storage::Store& s, rules::RuleEngine& r, ApiServices& h) : store(s), rules(r), services(h) {}

    bool authorized(const httplib::Request& req, std::string& request_id, httplib::Response& res) const {
        if (admin_token.empty()) {
            res.status = 503;
            res.body = error_body(ErrorCode::NotImplemented, "admin endpoints are disabled: no token configured",
                                  request_id)
                           .dump();
            return false;
        }
        auto auth = req.get_header_value("Authorization");
        const std::string prefix = "Bearer ";
        if (auth.rfind(prefix, 0) != 0 || !constant_time_equals(auth.substr(prefix.size()), admin_token)) {
            res.status = 401;
            res.body = error_body(ErrorCode::InvalidArgument, "missing or invalid admin token", request_id).dump();
            return false;
        }
        return true;
    }

    void setup() {
        svr.set_payload_max_length(kMaxBodyBytes);

        svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            json body;
            body["status"] = "ok";
            res.body = body.dump();
        });

        svr.Get("/ready", [this](const httplib::Request&, httplib::Response& res) {
            json body;
            body["ready"] = services.ready();
            res.status = services.ready() ? 200 : 503;
            res.body = body.dump();
        });

        svr.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
            res.body = MetricsRegistry::instance().render_prometheus();
        });

        svr.Get("/api/v1/devices", [this](const httplib::Request&, httplib::Response& res) {
            auto devices = store.list_devices();
            if (!devices.ok()) {
                res.status = 500;
                res.body = error_body(devices.error().code, devices.error().message, "").dump();
                return;
            }
            json arr = json::array();
            for (const auto& d : devices.value()) {
                json j;
                j["id"] = d.id;
                j["timezone"] = d.timezone;
                json tags = json::object();
                for (const auto& [k, v] : d.tags)
                    tags[k] = v;
                j["tags"] = tags;
                j["enabled"] = d.enabled;
                arr.push_back(std::move(j));
            }
            json body;
            body["devices"] = std::move(arr);
            res.status = 200;
            res.body = body.dump();
        });

        svr.Get("/api/v1/devices/(.*)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string request_id = req.get_header_value("X-Request-Id");
            auto devices = store.list_devices();
            if (!devices.ok()) {
                res.status = 500;
                res.body = error_body(devices.error().code, devices.error().message, request_id).dump();
                return;
            }
            for (const auto& d : devices.value()) {
                if (d.id == req.matches[1].str()) {
                    json j;
                    j["id"] = d.id;
                    j["timezone"] = d.timezone;
                    json tags = json::object();
                    for (const auto& [k, v] : d.tags)
                        tags[k] = v;
                    j["tags"] = tags;
                    j["enabled"] = d.enabled;
                    res.status = 200;
                    res.body = j.dump();
                    return;
                }
            }
            res.status = 404;
            res.body = error_body(ErrorCode::InvalidArgument, "device not found", request_id).dump();
        });

        svr.Get("/api/v1/samples", [this](const httplib::Request& req, httplib::Response& res) {
            handle_samples(req, res);
        });

        svr.Get("/api/v1/aggregates", [this](const httplib::Request& req, httplib::Response& res) {
            handle_aggregates(req, res);
        });

        svr.Get("/api/v1/incidents", [this](const httplib::Request& req, httplib::Response& res) {
            handle_incidents(req, res);
        });

        svr.Get("/api/v1/incidents/(.*)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string request_id = req.get_header_value("X-Request-Id");
            if (req.matches[1].str() == "ack") // POST-only subresource safety
            {
                res.status = 404;
                res.body = error_body(ErrorCode::InvalidArgument, "not found", request_id).dump();
                return;
            }
            auto incident = store.get_incident(req.matches[1].str());
            if (!incident.ok()) {
                res.status = 500;
                res.body = error_body(incident.error().code, incident.error().message, request_id).dump();
                return;
            }
            if (!incident.value().has_value()) {
                res.status = 404;
                res.body = error_body(ErrorCode::InvalidArgument, "incident not found", request_id).dump();
                return;
            }
            json body = incident_json(*incident.value());
            json timeline = json::array();
            auto entries = store.incident_timeline(incident.value()->uuid);
            if (entries.ok()) {
                for (const auto& e : entries.value()) {
                    json t;
                    t["at"] = iso8601_us(e.at_us);
                    t["from_state"] = e.from_state;
                    t["to_state"] = e.to_state;
                    t["reason"] = e.reason;
                    if (!e.actor.empty())
                        t["actor"] = e.actor;
                    timeline.push_back(std::move(t));
                }
            }
            body["timeline"] = std::move(timeline);
            res.status = 200;
            res.body = body.dump();
        });

        svr.Post("/api/v1/incidents/(.*)/ack", [this](const httplib::Request& req, httplib::Response& res) {
            std::string request_id = req.get_header_value("X-Request-Id");
            if (!authorized(req, request_id, res))
                return;
            json parsed = json::parse(req.body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("by")) {
                res.status = 400;
                res.body = error_body(ErrorCode::InvalidArgument, "body must be JSON with a 'by' field", request_id)
                               .dump();
                return;
            }
            std::string by = parsed.value("by", std::string());
            std::string comment = parsed.value("comment", std::string());
            auto acked = store.ack_incident(req.matches[1].str(), by, comment, now_unix_us());
            if (!acked.ok()) {
                res.status = 500;
                res.body = error_body(acked.error().code, acked.error().message, request_id).dump();
                return;
            }
            json body;
            body["updated"] = acked.value();
            res.status = acked.value() ? 200 : 404;
            if (!acked.value())
                body["message"] = "incident not found";
            res.body = body.dump();
        });

        svr.Post("/api/v1/admin/reload", [this](const httplib::Request& req, httplib::Response& res) {
            std::string request_id = req.get_header_value("X-Request-Id");
            if (!authorized(req, request_id, res))
                return;
            auto reloaded = services.reload_config(request_id, "api");
            if (!reloaded.ok()) {
                res.status = 400;
                res.body = error_body(reloaded.error().code, reloaded.error().message, request_id).dump();
                return;
            }
            json body;
            body["ok"] = true;
            auto cs = services.current_config();
            body["config_version"] = static_cast<int64_t>(cs ? cs->version : 0);
            res.status = 200;
            res.body = body.dump();
        });

        svr.Post("/api/v1/admin/import", [this](const httplib::Request& req, httplib::Response& res) {
            std::string request_id = req.get_header_value("X-Request-Id");
            if (!authorized(req, request_id, res))
                return;
            json parsed = json::parse(req.body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("path")) {
                res.status = 400;
                res.body = error_body(ErrorCode::InvalidArgument, "body must be JSON with a 'path' field", request_id)
                               .dump();
                return;
            }
            auto submitted = services.submit_import(parsed.value("path", std::string()),
                                                    parsed.value("recursive", false));
            if (!submitted.ok()) {
                res.status = 400;
                res.body = error_body(submitted.error().code, submitted.error().message, request_id).dump();
                return;
            }
            json body;
            body["job_id"] = submitted.value();
            res.status = 202;
            res.body = body.dump();
        });

        svr.Get("/api/v1/admin/jobs/(.*)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string request_id = req.get_header_value("X-Request-Id");
            if (!authorized(req, request_id, res))
                return;
            auto info = services.job(req.matches[1].str());
            json body;
            body["job_id"] = info.id;
            const char* states[] = {"queued", "running", "completed", "failed"};
            body["state"] = states[static_cast<int>(info.state)];
            body["path"] = info.path;
            body["accepted"] = info.accepted;
            body["total"] = info.total;
            if (!info.error.empty())
                body["error"] = info.error;
            res.status = 200;
            res.body = body.dump();
        });
    }

    void handle_samples(const httplib::Request& req, httplib::Response& res) {
        std::string request_id = req.get_header_value("X-Request-Id");
        RequestContext rc{request_id};
        auto device = req.get_param_value("device");
        auto metric = req.get_param_value("metric");
        if (device.empty() || metric.empty()) {
            res.status = 400;
            res.body = error_body(ErrorCode::InvalidArgument, "'device' and 'metric' are required", request_id).dump();
            return;
        }
        auto from = time_param(req, "from", true, rc, res);
        if (!from)
            return;
        auto to = time_param(req, "to", true, rc, res);
        if (!to)
            return;
        if (*from >= *to) {
            res.status = 400;
            res.body = error_body(ErrorCode::InvalidArgument, "'from' must be before 'to'", request_id).dump();
            return;
        }
        auto cs = services.current_config();
        const int64_t max_range = cs ? cs->cfg.server.max_query_range_us : 24LL * 3600 * 1000000;
        if (*to - *from > max_range) {
            res.status = 400;
            res.body = error_body(ErrorCode::InvalidArgument, "query range exceeds the configured maximum",
                                  request_id)
                           .dump();
            return;
        }
        auto limit = int_param(req, "limit", 100, rc, res);
        if (!limit)
            return;
        *limit = std::clamp<int64_t>(*limit, 1, 1000); // FR-API-001: default 100, max 1000
        int64_t after_id = 0;
        if (req.has_param("cursor")) {
            auto cursor = int_param(req, "cursor", 0, rc, res);
            if (!cursor)
                return;
            after_id = *cursor;
        }
        auto page = store.query_samples_page(device, metric, *from, *to, after_id, *limit);
        if (!page.ok()) {
            res.status = 500;
            res.body = error_body(page.error().code, page.error().message, request_id).dump();
            return;
        }
        json arr = json::array();
        for (const auto& s : page.value().rows) {
            json j;
            j["id"] = s.sample_uuid;
            j["device_id"] = s.device_id;
            j["metric_id"] = s.metric_id;
            j["event_time"] = iso8601_us(s.event_time_us);
            j["value"] = s.value_is_null ? json() : json(s.value);
            j["unit"] = s.input_unit;
            j["quality"] = s.quality;
            j["derived"] = (s.flags & sample_flags::kDerived) != 0;
            arr.push_back(std::move(j));
        }
        json body;
        body["samples"] = std::move(arr);
        if (page.value().has_more)
            body["next_cursor"] = std::to_string(page.value().last_id);
        res.status = 200;
        res.body = body.dump();
    }

    void handle_aggregates(const httplib::Request& req, httplib::Response& res) {
        std::string request_id = req.get_header_value("X-Request-Id");
        RequestContext rc{request_id};
        auto from = time_param(req, "from", true, rc, res);
        if (!from)
            return;
        auto to = time_param(req, "to", true, rc, res);
        if (!to)
            return;
        if (*from >= *to) {
            res.status = 400;
            res.body = error_body(ErrorCode::InvalidArgument, "'from' must be before 'to'", request_id).dump();
            return;
        }
        auto limit = int_param(req, "limit", 100, rc, res);
        if (!limit)
            return;
        *limit = std::clamp<int64_t>(*limit, 1, 1000);
        auto rows = store.query_aggregates(req.get_param_value("device"), req.get_param_value("metric"),
                                           req.get_param_value("window"), *from, *to, *limit);
        if (!rows.ok()) {
            res.status = 500;
            res.body = error_body(rows.error().code, rows.error().message, request_id).dump();
            return;
        }
        json arr = json::array();
        for (const auto& a : rows.value()) {
            json j;
            j["device_id"] = a.device_id;
            j["metric_id"] = a.metric_id;
            j["window"] = a.window_type;
            j["start"] = iso8601_us(a.start_us);
            j["end"] = iso8601_us(a.end_us);
            j["version"] = a.version;
            j["sample_count"] = a.sample_count;
            j["valid_count"] = a.valid_count;
            j["missing_count"] = a.missing_count;
            j["synthetic_count"] = a.synthetic_count;
            if (a.avg_value)
                j["avg"] = *a.avg_value;
            if (a.min_value)
                j["min"] = *a.min_value;
            if (a.max_value)
                j["max"] = *a.max_value;
            if (a.stddev)
                j["stddev"] = *a.stddev;
            if (a.p50)
                j["p50"] = *a.p50;
            if (a.p95)
                j["p95"] = *a.p95;
            if (a.p99)
                j["p99"] = *a.p99;
            arr.push_back(std::move(j));
        }
        json body;
        body["aggregates"] = std::move(arr);
        res.status = 200;
        res.body = body.dump();
    }

    void handle_incidents(const httplib::Request& req, httplib::Response& res) {
        std::string request_id = req.get_header_value("X-Request-Id");
        storage::IncidentFilter filter;
        filter.state = req.get_param_value("state");
        filter.severity = req.get_param_value("severity");
        filter.device_id = req.get_param_value("device");
        filter.rule_id = req.get_param_value("rule");
        if (req.has_param("from") || req.has_param("to")) {
            RequestContext rc{request_id};
            auto from = time_param(req, "from", false, rc, res);
            if (!from && req.has_param("from"))
                return;
            auto to = time_param(req, "to", false, rc, res);
            if (!to && req.has_param("to"))
                return;
            filter.from_us = from.value_or(0);
            filter.to_us = to.value_or(0);
        }
        auto limit = int_param(req, "limit", 100, rc2(request_id), res);
        if (!limit)
            return;
        filter.limit = std::clamp<int64_t>(*limit, 1, 1000);
        auto rows = store.query_incidents_filtered(filter);
        if (!rows.ok()) {
            res.status = 500;
            res.body = error_body(rows.error().code, rows.error().message, request_id).dump();
            return;
        }
        json arr = json::array();
        for (const auto& r : rows.value())
            arr.push_back(incident_json(r));
        json body;
        body["incidents"] = std::move(arr);
        res.status = 200;
        res.body = body.dump();
    }

    static RequestContext rc2(std::string id) {
        return RequestContext{std::move(id)};
    }
};

HttpApi::HttpApi(storage::Store& store, rules::RuleEngine& rules, ApiServices& services)
    : impl_(std::make_unique<Impl>(store, rules, services)) {
    impl_->setup();
}

HttpApi::~HttpApi() {
    stop();
}

Result<void> HttpApi::start(const HttpConfig& cfg, std::string admin_token) {
    impl_->admin_token = std::move(admin_token);
    auto& svr = impl_->svr;
    if (cfg.port == 0) {
        int bound = svr.bind_to_any_port(cfg.bind);
        if (bound <= 0)
            return Result<void>::Err(
                Error::make(ErrorCode::IoOpen, "http bind failed on " + cfg.bind).ctx("port", std::to_string(cfg.port)));
        impl_->bound_port = bound;
        impl_->worker = std::thread([&svr, bound, bind = cfg.bind]() { svr.listen_after_bind(); (void)bind; (void)bound; });
    } else {
        if (!svr.bind_to_port(cfg.bind, cfg.port))
            return Result<void>::Err(Error::make(ErrorCode::IoOpen, "http bind failed on " + cfg.bind)
                                         .ctx("port", std::to_string(cfg.port)));
        impl_->bound_port = cfg.port;
        impl_->worker = std::thread([&svr]() { svr.listen_after_bind(); });
    }
    MetricsRegistry::instance().set_gauge("sf_http_up", {}, 1.0);
    return Result<void>::Ok();
}

void HttpApi::stop() {
    impl_->svr.stop();
    if (impl_->worker.joinable())
        impl_->worker.join();
}

int HttpApi::bound_port() const {
    return impl_->bound_port;
}

} // namespace api
} // namespace streamforge
