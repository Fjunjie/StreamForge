#include "streamforge/ingest/archive.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "streamforge/core/fs_util.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/ingest/file_identity.hpp"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

namespace streamforge {
namespace fs = std::filesystem;

namespace {

// Copies `src` to `dst` (must not exist) and verifies the copy by size and SHA-256 of the
// first 64 KiB; used when a move cannot cross filesystems (requirement FR-IN-004).
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

struct MoveOutcome {
    bool moved = false;        // source successfully moved to dest
    bool dest_exists = false;  // dest already existed; nothing was changed
    bool cross_device = false; // atomic move impossible across filesystems
    bool failed = false;
    Error error;

    static MoveOutcome ok_moved() { return MoveOutcome{true, false, false, false, {}}; }
    static MoveOutcome exists() { return MoveOutcome{false, true, false, false, {}}; }
    static MoveOutcome exdev() { return MoveOutcome{false, false, true, false, {}}; }
    static MoveOutcome failure(Error e) { return MoveOutcome{false, false, false, true, std::move(e)}; }
};

// Atomically moves src to dest WITHOUT replacing an existing destination (FR-IN-004 /
// CWE-367): plain rename() would silently overwrite, so renameat2(RENAME_NOREPLACE) is
// used, with an atomic link()+unlink() fallback where the flag is unsupported.
MoveOutcome atomic_move_no_replace(const fs::path& src, const fs::path& dest) {
    if (::renameat2(AT_FDCWD, src.c_str(), AT_FDCWD, dest.c_str(), RENAME_NOREPLACE) == 0) {
        return MoveOutcome::ok_moved();
    }
    switch (errno) {
    case EEXIST:
    case ENOTEMPTY:
        return MoveOutcome::exists();
    case EXDEV:
        return MoveOutcome::exdev();
    case ENOSYS:
    case EINVAL:
    case EOPNOTSUPP:
        break; // flag unsupported: fall through to the link() based path
    default:
        return MoveOutcome::failure(
            Error::make(ErrorCode::IoRename, std::string("renameat2 failed: ") + std::strerror(errno))
                .ctx("src", src.string())
                .ctx("dest", dest.string()));
    }

    // link() is atomic and never replaces an existing destination.
    if (::link(src.c_str(), dest.c_str()) != 0) {
        switch (errno) {
        case EEXIST:
            return MoveOutcome::exists();
        case EXDEV:
            return MoveOutcome::exdev();
        default:
            return MoveOutcome::failure(
                Error::make(ErrorCode::IoRename, std::string("link failed: ") + std::strerror(errno))
                    .ctx("src", src.string())
                    .ctx("dest", dest.string()));
        }
    }
    std::error_code ec;
    fs::remove(src, ec);
    if (ec) {
        // The content is safe at dest, but the source could not be removed; surface the
        // error so the caller keeps both states visible instead of losing the file.
        return MoveOutcome::failure(Error::make(ErrorCode::IoRemove, "moved but cannot delete source file")
                                        .ctx("src", src.string())
                                        .ctx("detail", ec.message()));
    }
    return MoveOutcome::ok_moved();
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
    for (int attempt = 0;; ++attempt) {
        auto moved = atomic_move_no_replace(src, dest);
        if (moved.failed) {
            return Result<std::string>::Err(std::move(moved.error));
        }
        if (moved.moved) {
            break;
        }
        if (moved.cross_device) {
            // Cross-filesystem: copy + verify + delete; copy_file with copy_options::none
            // refuses to overwrite, preserving the no-clobber contract.
            auto copied = copy_and_verify(src, dest);
            if (!copied.ok()) {
                return Result<std::string>::Err(copied.error());
            }
            fs::remove(src, ec);
            if (ec) {
                return Result<std::string>::Err(Error::make(ErrorCode::IoRemove, "copied but cannot delete source file")
                                                    .ctx("src", path)
                                                    .ctx("detail", ec.message()));
            }
            break;
        }
        // Destination exists: append a short identity hash before the extension and retry
        // exactly once; a second collision is reported instead of overwritten.
        if (attempt >= 1) {
            return Result<std::string>::Err(
                Error::make(ErrorCode::PathCollision, "destination already exists; not overwriting")
                    .ctx("dest", dest.string()));
        }
        std::string stem = dest.stem().string();
        std::string ext = dest.extension().string();
        std::string collided = stem;
        collided += "-";
        collided += identity_hash.substr(0, 8);
        collided += ext;
        dest = dest_dir / collided;
    }

    if (quarantine) {
        fs::path err_file = dest_dir / (dest.filename().string() + ".error.json");
        fs::path tmp = err_file;
        tmp += ".tmp";
        bool write_ok = false;
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (f != nullptr) {
            // Every step is checked (CWE-252): a silently truncated report would leave the
            // quarantined file without its required diagnostics.
            write_ok = std::fwrite(error_json.data(), 1, error_json.size(), f) == error_json.size() &&
                       std::fflush(f) == 0 && ::fsync(::fileno(f)) == 0;
            if (std::fclose(f) != 0)
                write_ok = false;
        }
        if (!write_ok) {
            fs::remove(tmp, ec);
            return Result<std::string>::Err(
                Error::make(ErrorCode::IoWrite, "cannot write error report").ctx("dest", err_file.string()));
        }
        fs::rename(tmp, err_file, ec);
        if (ec) {
            fs::remove(tmp, ec);
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
