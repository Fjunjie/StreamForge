#include "streamforge/core/log.hpp"

#include <cstdio>
#include <map>
#include <mutex>
#include <sstream>

#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <date/date.h>

#include "streamforge/core/time.hpp"

namespace streamforge {
namespace {

class JsonFormatter : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        std::ostringstream ts;
        ts << date::format("%FT%TZ", date::floor<std::chrono::milliseconds>(msg.time));
        auto level_view = spdlog::level::to_string_view(msg.level);
        std::string level(level_view.begin(), level_view.end());
        std::string component(msg.logger_name.begin(), msg.logger_name.end());
        std::string text(msg.payload.begin(), msg.payload.end());
        fmt::format_to(std::back_inserter(dest),
                       "{{\"ts\":\"{}\",\"level\":\"{}\",\"component\":\"{}\",\"thread\":{},"
                       "\"msg\":\"{}\"}}\n",
                       escape(ts.str()), escape(level), escape(component), msg.thread_id, escape(text));
    }

    [[nodiscard]] std::unique_ptr<spdlog::formatter> clone() const override {
        return std::make_unique<JsonFormatter>(*this);
    }

private:
    static std::string escape(const std::string& in) {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
            }
        }
        return out;
    }
};

spdlog::level::level_enum parse_level(const std::string& name) {
    if (name == "trace")
        return spdlog::level::trace;
    if (name == "debug")
        return spdlog::level::debug;
    if (name == "info")
        return spdlog::level::info;
    if (name == "warn" || name == "warning")
        return spdlog::level::warn;
    if (name == "error")
        return spdlog::level::err;
    if (name == "critical")
        return spdlog::level::critical;
    if (name == "off")
        return spdlog::level::off;
    return spdlog::level::info;
}

struct LogState {
    std::mutex mu;
    std::vector<spdlog::sink_ptr> sinks;
    spdlog::level::level_enum level = spdlog::level::info;
    std::string format = "text";
    std::map<std::string, std::shared_ptr<spdlog::logger>> loggers;
};

LogState& state() {
    static LogState s;
    return s;
}

std::shared_ptr<spdlog::logger> make_logger(const std::string& component) {
    auto& s = state();
    auto lg = std::make_shared<spdlog::logger>(component, s.sinks.begin(), s.sinks.end());
    lg->set_level(s.level);
    if (s.format == "json") {
        lg->set_formatter(std::make_unique<JsonFormatter>());
    } else {
        lg->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [%t] %v");
    }
    return lg;
}

} // namespace

void init_logging(const LogConfig& cfg) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mu);

    std::vector<spdlog::sink_ptr> sinks;
    if (!cfg.quiet_console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_mt>());
    }
    if (!cfg.file.empty()) {
        try {
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                cfg.file, static_cast<size_t>(cfg.max_size_mb) * 1024 * 1024, cfg.max_files));
        } catch (const spdlog::spdlog_ex& ex) {
            // File sink is best effort; console logging still works.
            std::fprintf(stderr, "streamforge: cannot open log file %s: %s\n", cfg.file.c_str(), ex.what());
        }
    }
    if (sinks.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_mt>());
    }

    s.sinks = std::move(sinks);
    s.level = parse_level(cfg.level);
    s.format = cfg.format;
    s.loggers.clear();
}

std::shared_ptr<spdlog::logger> logger(const std::string& component) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.sinks.empty()) {
        // Default console logging until init_logging() is called (requirement documented in
        // log.hpp): early CLI/server errors must never vanish.
        s.sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_mt>());
        s.level = spdlog::level::info;
        s.format = "text";
    }
    auto it = s.loggers.find(component);
    if (it != s.loggers.end())
        return it->second;
    auto lg = make_logger(component);
    s.loggers[component] = lg;
    return lg;
}

void shutdown_logging() {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    for (auto& [name, lg] : s.loggers) {
        lg->flush();
    }
    s.loggers.clear();
}

} // namespace streamforge
