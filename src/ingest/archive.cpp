#include "streamforge/ingest/archive.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>

#include "streamforge/core/fs_util.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/ingest/file_identity.hpp"

namespace streamforge {
namespace fs = std::filesystem;

namespace {

// Copies `src` to `dst` (must not exist) and verifies the copy by size and SHA-256 of the
// first 64 KiB; used when rename() cannot cross filesystems (requirement FR-IN-004).
Result<void> copy_and_verify(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::none, ec);
    if (ec) {
        return Result<void>::Err(Error::make(ErrorCode::IoWrite, "cross-device copy failed")
                                     .ctx("src", src.string())
                                     .ctx("dst", dst.string())
                                     .ctx("detail", ec.message()));
    }
    auto ident_src = compute_file_identity(src.string());
    auto ident_dst = compute_file_identity(dst.string());
    if (!ident_src.ok() || !ident_dst.ok() || ident_src.value().size_bytes != ident_dst.value().size_bytes ||
        ident_src.value().sha256_64k != ident_dst.value().sha256_64k) {
        fs::remove(dst, ec);
        return Result<void>::Err(Error::make(ErrorCode::IoWrite, "copy verification failed; destination removed"));
    }
    return Result<void>::Ok();
}

Result<std::string> move_into(const std::string& path, const std::string& root, const std::string& identity_hash,
                              bool quarantine, const std::string& error_json) {
    std::error_code ec;
    fs::create_directories(fs::path(root), ec);
    if (ec && !fs::is_directory(fs::path(root))) {
        return Result<std::string>::Err(Error::make(ErrorCode::IoWrite, "cannot create target directory")
                                            .ctx("dir", root)
                                            .ctx("detail", ec.message()));
    }

    std::string sub;
    if (quarantine) {
        sub = ""; // quarantine root, flat (report sits next to the file)
    } else {
        std::string utc_date = format_utc_us(now_time()).substr(0, 10); // YYYY-MM-DD
        sub = utc_date.substr(0, 4) + "/" + utc_date.substr(5, 2) + "/" + utc_date.substr(8, 2);
    }
    fs::path dest_dir = fs::path(root) / sub;
    fs::create_directories(dest_dir, ec);
    if (ec && !fs::is_directory(dest_dir)) {
        return Result<std::string>::Err(Error::make(ErrorCode::IoWrite, "cannot create dated directory")
                                            .ctx("dir", dest_dir.string())
                                            .ctx("detail", ec.message()));
    }

    fs::path src(path);
    fs::path dest = dest_dir / src.filename();
    if (fs::exists(fs::symlink_status(dest, ec))) {
        // Name collision: append a short identity hash before the extension.
        std::string stem = dest.stem().string();
        std::string ext = dest.extension().string();
        dest = dest_dir / (stem + "-" + identity_hash.substr(0, 8) + ext);
    }
    if (fs::exists(fs::symlink_status(dest, ec))) {
        return Result<std::string>::Err(
            Error::make(ErrorCode::PathCollision, "destination already exists; not overwriting")
                .ctx("dest", dest.string()));
    }

    errno = 0;
    fs::rename(src, dest, ec);
    if (ec) {
        bool cross_device = ec == std::errc::cross_device_link;
        if (!cross_device) {
            return Result<std::string>::Err(Error::make(ErrorCode::IoRename, "rename failed")
                                                .ctx("src", path)
                                                .ctx("dest", dest.string())
                                                .ctx("detail", ec.message()));
        }
        auto copied = copy_and_verify(src, dest);
        if (!copied.ok())
            return Result<std::string>::Err(copied.error());
        fs::remove(src, ec);
        if (ec) {
            return Result<std::string>::Err(Error::make(ErrorCode::IoRemove, "copied but cannot delete source file")
                                                .ctx("src", path)
                                                .ctx("detail", ec.message()));
        }
    }

    if (quarantine) {
        fs::path err_file = dest_dir / (dest.filename().string() + ".error.json");
        std::error_code wec;
        fs::path tmp = err_file;
        tmp += ".tmp";
        {
            std::FILE* f = std::fopen(tmp.c_str(), "wb");
            if (f == nullptr) {
                return Result<std::string>::Err(
                    Error::make(ErrorCode::IoWrite, "cannot write error report").ctx("dest", err_file.string()));
            }
            std::fwrite(error_json.data(), 1, error_json.size(), f);
            std::fflush(f);
            std::fclose(f);
        }
        fs::rename(tmp, err_file, wec);
        if (wec) {
            return Result<std::string>::Err(
                Error::make(ErrorCode::IoWrite, "cannot finalize error report").ctx("dest", err_file.string()));
        }
    }
    return Result<std::string>::Ok(dest.string());
}

} // namespace

Result<std::string> archive_file(const std::string& path, const std::string& archive_root,
                                 const std::string& identity_hash) {
    return move_into(path, archive_root, identity_hash, false, "");
}

Result<std::string> quarantine_file(const std::string& path, const std::string& quarantine_root,
                                    const std::string& identity_hash, const std::string& error_json) {
    return move_into(path, quarantine_root, identity_hash, true, error_json);
}

} // namespace streamforge
