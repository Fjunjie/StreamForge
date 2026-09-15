#include "streamforge/ingest/file_identity.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <openssl/evp.h>

#include "streamforge/core/fs_util.hpp"

namespace streamforge {
namespace fs = std::filesystem;

std::string sha256_hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr)
        return {};
    std::string out;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 && EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1) {
        char buf[3];
        for (unsigned int i = 0; i < digest_len; ++i) {
            std::snprintf(buf, sizeof(buf), "%02x", digest[i]);
            out += buf;
        }
    }
    EVP_MD_CTX_free(ctx);
    return out;
}

Result<FileIdentity> compute_file_identity(const std::string& path) {
    // Symlinks are rejected before any path resolution so the final component cannot be a
    // link (requirement 10.3; directory discovery skips symlinks as well).
    std::error_code sec;
    if (fs::is_symlink(fs::symlink_status(fs::path(path), sec))) {
        return Result<FileIdentity>::Err(
            Error::make(ErrorCode::PathInvalid, "symlinks are not followed").ctx("path", path));
    }

    auto canonical = canonical_abs(path);
    if (!canonical.ok()) {
        return Result<FileIdentity>::Err(canonical.error());
    }

    std::error_code ec;
    auto status = fs::status(fs::path(canonical.value()), ec);
    if (ec || !fs::is_regular_file(status)) {
        return Result<FileIdentity>::Err(
            Error::make(ErrorCode::IoStat, "not a regular file").ctx("path", canonical.value()));
    }

    // POSIX stat for a stable microsecond mtime.
    struct ::stat st{};
    if (::stat(canonical.value().c_str(), &st) != 0) {
        return Result<FileIdentity>::Err(Error::make(ErrorCode::IoStat, "stat failed").ctx("path", canonical.value()));
    }
    FileIdentity identity;
    identity.path = canonical.value();
    identity.size_bytes = static_cast<int64_t>(st.st_size);
    identity.mtime_us = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000 + st.st_mtim.tv_nsec / 1000;

    // SHA-256 over the first 64 KiB.
    std::ifstream in(canonical.value(), std::ios::binary);
    if (!in) {
        return Result<FileIdentity>::Err(
            Error::make(ErrorCode::IoOpen, "cannot open file for hashing").ctx("path", canonical.value()));
    }
    std::string head(64ULL * 1024, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    identity.sha256_64k = sha256_hex(head);
    if (identity.sha256_64k.empty()) {
        return Result<FileIdentity>::Err(
            Error::make(ErrorCode::IoRead, "sha256 computation failed").ctx("path", identity.path));
    }

    // Combined identity hash.
    std::string combined;
    combined += identity.path;
    combined += '\0';
    combined += std::to_string(identity.size_bytes);
    combined += '\0';
    combined += std::to_string(identity.mtime_us);
    combined += '\0';
    combined += identity.sha256_64k;
    identity.identity_hash = sha256_hex(combined);
    return Result<FileIdentity>::Ok(std::move(identity));
}

} // namespace streamforge
