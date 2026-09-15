#include "streamforge/ingest/discovery.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>

#include <spdlog/spdlog.h>

#include "streamforge/core/log.hpp"
#include "streamforge/core/time.hpp"

namespace streamforge {
namespace fs = std::filesystem;

DirectoryScanner::DirectoryScanner(std::string dir, std::chrono::milliseconds quiet_period)
    : dir_(std::move(dir)), quiet_period_(quiet_period) {}

namespace {

bool is_hidden_or_temp(const std::string& name) {
    if (name.empty() || name[0] == '.')
        return true;
    static const char* kTempSuffixes[] = {".tmp", ".part", ".partial", ".swp", "~"};
    for (const char* suffix : kTempSuffixes) {
        size_t len = std::strlen(suffix);
        if (name.size() > len && name.compare(name.size() - len, len, suffix) == 0)
            return true;
    }
    return false;
}

int64_t stat_mtime_us(const fs::path& p, int64_t& size_out) {
    struct ::stat st{};
    if (::stat(p.c_str(), &st) != 0)
        return -1;
    size_out = static_cast<int64_t>(st.st_size);
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000 + st.st_mtim.tv_nsec / 1000;
}

} // namespace

Result<std::vector<ScanHit>> DirectoryScanner::scan() {
    std::error_code ec;
    fs::directory_iterator it(fs::path(dir_), fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return Result<std::vector<ScanHit>>::Err(
            Error::make(ErrorCode::IoOpen, "cannot scan input directory").ctx("dir", dir_).ctx("detail", ec.message()));
    }

    const int64_t now_us = now_unix_us();
    std::map<std::string, Observed> next_observed;
    std::vector<ScanHit> hits;

    for (const auto& entry : it) {
        std::string name = entry.path().filename().string();
        std::string full = entry.path().string();
        if (fs::is_symlink(fs::symlink_status(entry.path(), ec))) {
            SPDLOG_LOGGER_DEBUG(logger("ingest"), "skipping symlink: {}", full);
            continue;
        }
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec)
            continue;
        if (is_hidden_or_temp(name)) {
            SPDLOG_LOGGER_DEBUG(logger("ingest"), "ignoring hidden/temp file: {}", full);
            continue;
        }

        bool via_ready = false;
        std::string effective = strip_ready_suffix(name);
        if (effective != name) {
            via_ready = true;
            if (effective.empty())
                continue; // a bare ".ready" is meaningless
        }
        size_t dot = effective.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? "" : effective.substr(dot + 1);
        InputFormat fmt = format_from_extension(ext);
        if (fmt == InputFormat::Unknown) {
            SPDLOG_LOGGER_DEBUG(logger("ingest"), "ignoring unsupported extension: {}", full);
            continue;
        }
        if (fmt == InputFormat::Tlm) {
            // TLM processing arrives in M2; discovered but not yet processed.
            SPDLOG_LOGGER_DEBUG(logger("ingest"), "TLM file deferred to M2: {}", full);
            continue;
        }

        int64_t size = 0;
        int64_t mtime = stat_mtime_us(entry.path(), size);
        if (mtime < 0)
            continue;

        Observed obs;
        obs.size = size;
        obs.mtime_us = mtime;
        obs.seen_before = true; // recorded for the next scan's invariance check
        next_observed[full] = obs;

        if (via_ready) {
            if (reported_.insert(full).second) {
                hits.push_back(ScanHit{full, true, fmt});
            }
            continue;
        }
        auto prev = observed_.find(full);
        bool unchanged = prev != observed_.end() && prev->second.seen_before && prev->second.size == size &&
                         prev->second.mtime_us == mtime;
        bool quiet = (now_us - mtime) >= quiet_period_.count() * 1000;
        if (unchanged && quiet && reported_.insert(full).second) {
            hits.push_back(ScanHit{full, false, fmt});
        }
    }

    observed_ = std::move(next_observed);
    std::sort(hits.begin(), hits.end(), [](const ScanHit& a, const ScanHit& b) { return a.path < b.path; });
    return Result<std::vector<ScanHit>>::Ok(std::move(hits));
}

} // namespace streamforge
