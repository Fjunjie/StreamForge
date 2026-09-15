#include "streamforge/core/fs_util.hpp"

#include <filesystem>

namespace streamforge {
namespace fs = std::filesystem;

Result<std::string> canonical_abs(const std::string& path) {
    if (path.empty()) {
        return Result<std::string>::Err(Error::make(ErrorCode::PathInvalid, "empty path").ctx("path", path));
    }
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(path), ec);
    if (ec) {
        return Result<std::string>::Err(
            Error::make(ErrorCode::PathInvalid, "cannot resolve absolute path").ctx("path", path));
    }
    fs::path canonical = fs::weakly_canonical(abs, ec);
    if (ec)
        canonical = abs; // final component may not exist yet; lexical normalization suffices
    std::string normalized = canonical.lexically_normal().string();
    if (normalized.empty()) {
        return Result<std::string>::Err(
            Error::make(ErrorCode::PathInvalid, "path normalizes to empty").ctx("path", path));
    }
    return Result<std::string>::Ok(normalized);
}

bool path_within(const std::string& root_canonical, const std::string& candidate_canonical) {
    if (root_canonical.empty() || candidate_canonical.empty())
        return false;
    // Defensive: normalize both sides so ".." components cannot escape containment.
    std::error_code ec;
    fs::path candidate = fs::weakly_canonical(fs::path(candidate_canonical), ec);
    if (ec)
        candidate = fs::path(candidate_canonical);
    std::string normalized = candidate.lexically_normal().string();
    if (normalized == root_canonical)
        return true;
    std::string prefix = root_canonical;
    if (prefix.back() != '/')
        prefix += '/';
    return normalized.compare(0, prefix.size(), prefix) == 0;
}

std::string sanitize_raw(const std::string& raw, size_t max_bytes) {
    std::string out;
    out.reserve(raw.size() < max_bytes ? raw.size() : max_bytes);
    for (char c : raw) {
        if (out.size() >= max_bytes)
            break;
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F)
            continue; // strip control characters
        out.push_back(c);
    }
    return out;
}

} // namespace streamforge
