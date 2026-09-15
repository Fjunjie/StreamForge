// streamforgectl: management, query, import and report CLI.
// M1 scope: validate-config, import, status, devices list, samples query, incidents
// list/ack, db migrate, db check. Report creation and replay arrive in M3/M4.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamforge/config/config.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/ingest/pipeline.hpp"
#include "streamforge/storage/store.hpp"

namespace fs = std::filesystem;

using nlohmann::json;
using streamforge::storage::Store;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitBusinessFailure = 1;
constexpr int kExitArgError = 2;

struct Args {
    std::string cmd;
    std::string positional1; // subcommand word ("list"/"query"/"ack"/"migrate"/"check") or path
    std::string positional2; // incident id for "ack"
    std::string config;
    std::string tag;
    std::string device, metric, from, to;
    std::string state, severity;
    std::string by, comment;
    int64_t limit = 1000;
    bool recursive = false;
    bool wait = false;
    bool exclude_synthetic = false;
    bool json_output = false;
};

void usage() {
    std::fputs("streamforgectl - StreamForge command line tool\n"
               "\n"
               "Usage:\n"
               "  streamforgectl validate-config --config <file> [--output json]\n"
               "  streamforgectl import <path> --config <file> [--recursive] [--wait]\n"
               "  streamforgectl status --config <file> [--output json]\n"
               "  streamforgectl devices list --config <file> [--tag key=value] [--output json]\n"
               "  streamforgectl samples query --config <file> --device <id> --metric <id>\n"
               "                 --from <time> --to <time> [--limit N] [--exclude-synthetic]\n"
               "  streamforgectl incidents list --config <file> [--state open] [--severity high]\n"
               "  streamforgectl incidents ack <incident-id> --config <file> --by <name>\n"
               "                 [--comment <text>]\n"
               "  streamforgectl db migrate --config <file>\n"
               "  streamforgectl db check --config <file>\n"
               "\n"
               "Exit codes: 0 success, 1 business failure, 2 argument error.\n",
               stderr);
}

// Parses "[word] [word2] [--flag value ...]" after the command word.
bool parse_args(int argc, char** argv, Args& args) {
    int positionals = 0;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name, std::string& dest) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return false;
            }
            dest = argv[++i];
            return true;
        };
        if (arg == "--config") {
            if (!need_value("--config", args.config))
                return false;
        } else if (arg == "--recursive") {
            args.recursive = true;
        } else if (arg == "--wait") {
            args.wait = true;
        } else if (arg == "--output") {
            std::string v;
            if (!need_value("--output", v))
                return false;
            args.json_output = (v == "json");
        } else if (arg == "--tag") {
            if (!need_value("--tag", args.tag))
                return false;
        } else if (arg == "--device") {
            if (!need_value("--device", args.device))
                return false;
        } else if (arg == "--metric") {
            if (!need_value("--metric", args.metric))
                return false;
        } else if (arg == "--from") {
            if (!need_value("--from", args.from))
                return false;
        } else if (arg == "--to") {
            if (!need_value("--to", args.to))
                return false;
        } else if (arg == "--limit") {
            std::string v;
            if (!need_value("--limit", v))
                return false;
            args.limit = std::strtoll(v.c_str(), nullptr, 10);
        } else if (arg == "--exclude-synthetic") {
            args.exclude_synthetic = true;
        } else if (arg == "--state") {
            if (!need_value("--state", args.state))
                return false;
        } else if (arg == "--severity") {
            if (!need_value("--severity", args.severity))
                return false;
        } else if (arg == "--by") {
            if (!need_value("--by", args.by))
                return false;
        } else if (arg == "--comment") {
            if (!need_value("--comment", args.comment))
                return false;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            return false;
        } else if (positionals == 0) {
            args.positional1 = arg;
            ++positionals;
        } else if (positionals == 1) {
            args.positional2 = arg;
            ++positionals;
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

std::shared_ptr<const streamforge::ConfigSnapshot> must_load_config(const Args& args) {
    if (args.config.empty()) {
        std::fprintf(stderr, "--config is required\n");
        return nullptr;
    }
    auto loaded = streamforge::load_config(args.config);
    if (!loaded.ok()) {
        std::fprintf(stderr, "invalid configuration: %s\n", loaded.error().message.c_str());
        for (const auto& [k, v] : loaded.error().context) {
            std::fprintf(stderr, "  %s: %s\n", k.c_str(), v.c_str());
        }
        return nullptr;
    }
    return loaded.value();
}

std::shared_ptr<Store> must_open_store(const streamforge::ConfigSnapshot& cs) {
    auto store = Store::open(cs.cfg.database.path);
    if (!store.ok()) {
        std::fprintf(stderr, "cannot open database: %s\n", store.error().message.c_str());
        return nullptr;
    }
    return store.value();
}

std::string iso_or_opt(std::optional<int64_t> us) {
    if (!us)
        return "";
    return streamforge::format_utc_us(streamforge::from_unix_us(*us));
}

std::string value_to_string(const streamforge::storage::SampleRow& r) {
    if (r.value_is_null)
        return "null";
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", r.value);
    return buf;
}

int cmd_validate_config(const Args& args, const streamforge::ConfigSnapshot& cs) {
    if (args.json_output) {
        json out;
        out["ok"] = true;
        out["config_version"] = cs.version;
        out["schema_version"] = cs.cfg.schema_version;
        std::printf("%s\n", out.dump().c_str());
    } else {
        std::printf("configuration OK (config_version=%llu)\n", static_cast<unsigned long long>(cs.version));
    }
    return kExitOk;
}

int cmd_import(const Args& args, const streamforge::ConfigSnapshot& cs, Store& store) {
    if (args.positional1.empty()) {
        std::fprintf(stderr, "import requires a file or directory path\n");
        return kExitArgError;
    }
    std::error_code ec;
    if (!fs::exists(fs::path(args.positional1), ec)) {
        std::fprintf(stderr, "path does not exist: %s\n", args.positional1.c_str());
        return kExitBusinessFailure;
    }

    std::vector<std::string> paths;
    if (fs::is_directory(fs::path(args.positional1))) {
        std::error_code itec;
        if (args.recursive) {
            fs::recursive_directory_iterator it(fs::path(args.positional1),
                                                fs::directory_options::skip_permission_denied, itec);
            fs::recursive_directory_iterator end;
            while (it != end) {
                std::error_code fec;
                if (it->is_regular_file(fec))
                    paths.push_back(it->path().string());
                it.increment(itec);
                if (itec)
                    break;
            }
        } else {
            fs::directory_iterator it(fs::path(args.positional1), fs::directory_options::skip_permission_denied, itec);
            for (const auto& entry : it) {
                std::error_code fec;
                if (entry.is_regular_file(fec))
                    paths.push_back(entry.path().string());
            }
        }
    } else {
        paths.push_back(args.positional1);
    }
    std::sort(paths.begin(), paths.end()); // lexicographic submission order (FR-IN-001)

    streamforge::ImportPipeline pipeline(std::make_shared<const streamforge::ConfigSnapshot>(cs),
                                         std::shared_ptr<Store>(&store, [](Store*) {}));
    size_t success = 0, skipped = 0, failed = 0;
    for (const auto& path : paths) {
        auto result = pipeline.process_file(path);
        switch (result.outcome) {
        case streamforge::ImportPipeline::ProcessResult::Outcome::Completed:
            ++success;
            std::fprintf(stderr, "ok         %s (accepted=%" PRId64 " errors=%" PRId64 "/%" PRId64 ")\n", path.c_str(),
                         result.accepted, result.format_errors + result.business_errors, result.total);
            break;
        case streamforge::ImportPipeline::ProcessResult::Outcome::Quarantined:
            ++failed;
            std::fprintf(stderr, "quarantine %s (business_errors=%" PRId64 " total=%" PRId64 ")\n", path.c_str(),
                         result.business_errors, result.total);
            break;
        case streamforge::ImportPipeline::ProcessResult::Outcome::SkippedDuplicate:
            ++skipped;
            std::fprintf(stderr, "skip       %s (identity already processed)\n", path.c_str());
            break;
        default:
            ++failed;
            std::fprintf(stderr, "failed     %s: %s\n", path.c_str(), result.error.message.c_str());
            break;
        }
    }

    if (args.json_output) {
        json out;
        out["success"] = success;
        out["skipped"] = skipped;
        out["failed"] = failed;
        std::printf("%s\n", out.dump().c_str());
    } else {
        std::printf("import finished: success=%zu skipped=%zu failed=%zu\n", success, skipped, failed);
    }
    json details;
    details["success"] = success;
    details["skipped"] = skipped;
    details["failed"] = failed;
    store.audit("import", "cli", args.positional1, failed > 0 ? "partial" : "ok", "", details.dump());
    return failed > 0 ? kExitBusinessFailure : kExitOk;
}

int schema_version_or(Store& store) {
    auto v = store.schema_version();
    return v.ok() ? v.value() : -1;
}

int cmd_status(const Args& args, const streamforge::ConfigSnapshot& cs, Store& store) {
    auto counts = store.file_status_counts();
    auto samples = store.count_all_samples();
    if (!counts.ok() || !samples.ok()) {
        std::fprintf(stderr, "status query failed\n");
        return kExitBusinessFailure;
    }
    if (args.json_output) {
        json out;
        out["schema_version"] = schema_version_or(store);
        out["config_version"] = cs.version;
        out["samples"] = samples.value();
        json files = json::object();
        for (const auto& [k, v] : counts.value())
            files[k] = v;
        out["files"] = std::move(files);
        std::printf("%s\n", out.dump().c_str());
    } else {
        std::printf("database: %s (schema v%d, config v%llu)\n", cs.cfg.database.path.c_str(), schema_version_or(store),
                    static_cast<unsigned long long>(cs.version));
        std::printf("samples: %lld\n", static_cast<long long>(samples.value()));
        std::printf("files:\n");
        for (const auto& [k, v] : counts.value()) {
            std::printf("  %-18s %lld\n", k.c_str(), static_cast<long long>(v));
        }
    }
    return kExitOk;
}

int cmd_devices(const Args& args, const streamforge::ConfigSnapshot& cs) {
    std::string tag_key, tag_value;
    if (!args.tag.empty()) {
        size_t eq = args.tag.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "--tag expects key=value\n");
            return kExitArgError;
        }
        tag_key = args.tag.substr(0, eq);
        tag_value = args.tag.substr(eq + 1);
    }
    auto matches = [&](const streamforge::DeviceCfg& d) {
        if (tag_key.empty())
            return true;
        for (const auto& t : d.tags) {
            if (t.first == tag_key && t.second == tag_value)
                return true;
        }
        return false;
    };
    if (args.json_output) {
        json arr = json::array();
        for (const auto& d : cs.cfg.devices) {
            if (!matches(d))
                continue;
            json item;
            item["id"] = d.id;
            item["timezone"] = d.timezone;
            json tags = json::object();
            for (const auto& t : d.tags)
                tags[t.first] = t.second;
            item["tags"] = std::move(tags);
            json metrics = json::array();
            for (const auto& m : d.metrics)
                metrics.push_back(m);
            item["metrics"] = std::move(metrics);
            arr.push_back(std::move(item));
        }
        std::printf("%s\n", json{{"devices", std::move(arr)}}.dump().c_str());
    } else {
        for (const auto& d : cs.cfg.devices) {
            if (!matches(d))
                continue;
            std::string tags;
            for (const auto& t : d.tags)
                tags += t.first + "=" + t.second + " ";
            std::printf("%-20s %-16s %s\n", d.id.c_str(), d.timezone.c_str(), tags.c_str());
        }
    }
    return kExitOk;
}

int cmd_samples_query(const Args& args, Store& store) {
    if (args.device.empty() || args.metric.empty() || args.from.empty() || args.to.empty()) {
        std::fprintf(stderr, "samples query requires --device, --metric, --from and --to\n");
        return kExitArgError;
    }
    auto from = streamforge::parse_admin_time(args.from);
    auto to = streamforge::parse_admin_time(args.to);
    if (!from.ok() || !to.ok()) {
        std::fprintf(stderr, "invalid --from/--to time\n");
        return kExitArgError;
    }
    streamforge::storage::SampleQuery q;
    q.device_id = args.device;
    q.metric_id = args.metric;
    q.from_us = streamforge::to_unix_us(from.value());
    q.to_us = streamforge::to_unix_us(to.value());
    q.limit = args.limit;
    q.exclude_synthetic = args.exclude_synthetic;
    auto rows = store.query_samples(q);
    if (!rows.ok()) {
        std::fprintf(stderr, "query failed: %s\n", rows.error().message.c_str());
        return kExitBusinessFailure;
    }
    if (args.json_output) {
        json arr = json::array();
        for (const auto& r : rows.value()) {
            json item;
            item["sample_id"] = r.sample_uuid;
            item["device_id"] = r.device_id;
            item["metric_id"] = r.metric_id;
            item["event_time"] = streamforge::format_utc_us(streamforge::from_unix_us(r.event_time_us));
            item["value"] = r.value_is_null ? json(nullptr) : json(r.value);
            if (!r.input_unit.empty())
                item["input_unit"] = r.input_unit;
            item["quality"] = r.quality;
            item["flags"] = r.flags;
            item["tags"] = json::parse(r.tags_json.empty() ? "{}" : r.tags_json);
            arr.push_back(std::move(item));
        }
        std::printf("%s\n", json{{"samples", std::move(arr)}}.dump().c_str());
    } else {
        for (const auto& r : rows.value()) {
            std::printf("%s %s=%s %s q=%d\n", r.device_id.c_str(), r.metric_id.c_str(), value_to_string(r).c_str(),
                        streamforge::format_utc_us(streamforge::from_unix_us(r.event_time_us)).c_str(), r.quality);
        }
        std::printf("(%zu rows)\n", rows.value().size());
    }
    return kExitOk;
}

int cmd_incidents_list(const Args& args, Store& store) {
    auto rows = store.query_incidents(args.state, args.severity, 1000);
    if (!rows.ok()) {
        std::fprintf(stderr, "query failed: %s\n", rows.error().message.c_str());
        return kExitBusinessFailure;
    }
    if (args.json_output) {
        json arr = json::array();
        for (const auto& r : rows.value()) {
            json item;
            item["incident_id"] = r.uuid;
            item["rule_id"] = r.rule_id;
            item["device_id"] = r.device_id;
            item["severity"] = r.severity;
            item["state"] = r.state;
            item["started"] = streamforge::format_utc_us(streamforge::from_unix_us(r.started_us));
            item["ended"] = iso_or_opt(r.ended_us);
            arr.push_back(std::move(item));
        }
        std::printf("%s\n", json{{"incidents", std::move(arr)}}.dump().c_str());
    } else {
        for (const auto& r : rows.value()) {
            std::printf("%s %s rule=%s dev=%s sev=%s started=%s\n", r.uuid.c_str(), r.state.c_str(), r.rule_id.c_str(),
                        r.device_id.c_str(), r.severity.c_str(),
                        streamforge::format_utc_us(streamforge::from_unix_us(r.started_us)).c_str());
        }
        std::printf("(%zu incidents)\n", rows.value().size());
    }
    return kExitOk;
}

int cmd_incidents_ack(const Args& args, Store& store) {
    if (args.positional2.empty()) {
        std::fprintf(stderr, "usage: streamforgectl incidents ack <incident-id> --by <name> "
                             "[--comment <text>]\n");
        return kExitArgError;
    }
    if (args.by.empty()) {
        std::fprintf(stderr, "--by is required for ack\n");
        return kExitArgError;
    }
    auto acked = store.ack_incident(args.positional2, args.by, args.comment, streamforge::now_unix_us());
    if (!acked.ok()) {
        std::fprintf(stderr, "ack failed: %s\n", acked.error().message.c_str());
        return kExitBusinessFailure;
    }
    if (!acked.value()) {
        std::fprintf(stderr, "incident not found: %s\n", args.positional2.c_str());
        return kExitBusinessFailure;
    }
    store.audit("incident_ack", "cli", args.positional2, "ok", "", "{}");
    if (!args.json_output)
        std::printf("incident %s acknowledged\n", args.positional2.c_str());
    return kExitOk;
}

int cmd_db_migrate(const Args& args, const streamforge::ConfigSnapshot& cs, Store& store) {
    int version = schema_version_or(store);
    if (args.json_output) {
        std::printf("%s\n", json{{"schema_version", version}}.dump().c_str());
    } else {
        std::printf("database schema version: %d\n", version);
    }
    store.audit("db_migrate", "cli", cs.cfg.database.path, "ok", "", "{}");
    return kExitOk;
}

int cmd_db_check(const Args& args, const streamforge::ConfigSnapshot& cs, Store& store) {
    auto integrity = store.integrity_check();
    auto fk = store.foreign_key_violations();
    if (!integrity.ok() || !fk.ok()) {
        std::fprintf(stderr, "check failed\n");
        return kExitBusinessFailure;
    }
    bool ok = integrity.value() == "ok" && fk.value() == 0;
    if (args.json_output) {
        json out;
        out["integrity_check"] = integrity.value();
        out["foreign_key_violations"] = fk.value();
        out["ok"] = ok;
        std::printf("%s\n", out.dump().c_str());
    } else {
        std::printf("integrity_check: %s\n", integrity.value().c_str());
        std::printf("foreign_key_violations: %lld\n", static_cast<long long>(fk.value()));
    }
    store.audit("db_check", "cli", cs.cfg.database.path, ok ? "ok" : "failed", "", "{}");
    return ok ? kExitOk : kExitBusinessFailure;
}

} // namespace

int run_ctl_main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return kExitArgError;
    }
    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h") {
        usage();
        return kExitOk;
    }
    {
        static const char* kCommands[] = {"validate-config", "import",    "status", "devices",
                                          "samples",         "incidents", "db"};
        bool known = false;
        for (const char* c : kCommands) {
            if (cmd == c)
                known = true;
        }
        if (!known) {
            std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
            usage();
            return kExitArgError;
        }
    }

    Args args;
    args.cmd = cmd;
    if (!parse_args(argc, argv, args)) {
        usage();
        return kExitArgError;
    }
    static const char* kNeedsConfig[] = {"validate-config", "import",    "status", "devices",
                                         "samples",         "incidents", "db"};
    for (const char* c : kNeedsConfig) {
        if (cmd == c && args.config.empty()) {
            std::fprintf(stderr, "--config is required for %s\n", cmd.c_str());
            return kExitArgError;
        }
    }

    // Commands that only need configuration.
    if (cmd == "validate-config") {
        auto cs = must_load_config(args);
        if (cs == nullptr)
            return kExitBusinessFailure;
        return cmd_validate_config(args, *cs);
    }

    // Everything else needs both configuration and a database.
    auto cs = must_load_config(args);
    if (cs == nullptr)
        return kExitBusinessFailure;
    auto store = must_open_store(*cs);
    if (store == nullptr)
        return kExitBusinessFailure;

    // Keep the catalog in step with the configuration for every write-path command.
    if (cmd == "import" || cmd == "db") {
        auto synced = store->sync_catalog(*cs);
        if (!synced.ok()) {
            std::fprintf(stderr, "catalog sync failed: %s\n", synced.error().message.c_str());
            return kExitBusinessFailure;
        }
    }

    if (cmd == "import")
        return cmd_import(args, *cs, *store);
    if (cmd == "status")
        return cmd_status(args, *cs, *store);
    if (cmd == "devices") {
        if (args.positional1 != "list") {
            std::fprintf(stderr, "usage: streamforgectl devices list [--tag key=value]\n");
            return kExitArgError;
        }
        return cmd_devices(args, *cs);
    }
    if (cmd == "samples") {
        if (args.positional1 != "query") {
            std::fprintf(stderr, "usage: streamforgectl samples query --device <id> --metric "
                                 "<id> --from <time> --to <time>\n");
            return kExitArgError;
        }
        return cmd_samples_query(args, *store);
    }
    if (cmd == "incidents") {
        if (args.positional1.empty() || args.positional1 == "list") {
            return cmd_incidents_list(args, *store);
        }
        if (args.positional1 == "ack")
            return cmd_incidents_ack(args, *store);
        std::fprintf(stderr, "usage: streamforgectl incidents [list|ack]\n");
        return kExitArgError;
    }
    if (cmd == "db") {
        if (args.positional1 == "migrate")
            return cmd_db_migrate(args, *cs, *store);
        if (args.positional1 == "check")
            return cmd_db_check(args, *cs, *store);
        std::fprintf(stderr, "usage: streamforgectl db [migrate|check]\n");
        return kExitArgError;
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    usage();
    return kExitArgError;
}

int main(int argc, char** argv) {
    try {
        return run_ctl_main(argc, argv);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "streamforgectl: fatal: %s\n", ex.what());
        return 1;
    }
}
