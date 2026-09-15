#pragma once

#include <cstdint>
#include <string>

#include "streamforge/core/error.hpp"

namespace streamforge {

// File identity per requirement FR-IN-003: normalized absolute path + size + last modification
// time + SHA-256 over the first 64 KiB of content. `identity_hash` is a SHA-256 over all four.
struct FileIdentity {
    std::string path; // normalized absolute path
    int64_t size_bytes = 0;
    int64_t mtime_us = 0;
    std::string sha256_64k; // lowercase hex
    std::string identity_hash;
};

Result<FileIdentity> compute_file_identity(const std::string& path);

// SHA-256 (hex, lowercase) over arbitrary bytes; implemented with OpenSSL EVP.
std::string sha256_hex(const std::string& data);

} // namespace streamforge
