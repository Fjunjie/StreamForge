#include "streamforge/storage/migrations.hpp"

#include <sqlite3.h>

#include <string>

#include "migrations_embedded.hpp"
#include "streamforge/core/error.hpp"
#include "streamforge/core/time.hpp"
#include "streamforge/storage/db.hpp"

namespace streamforge {
namespace storage {
namespace embedded_migrations {
using migrations::EmbeddedMigration;
using migrations::kMigrationCount;
using migrations::kMigrations;
} // namespace embedded_migrations

Result<int> current_version(Db& db) {
    auto rc = db.exec("CREATE TABLE IF NOT EXISTS schema_migrations ("
                      "version INTEGER PRIMARY KEY, name TEXT NOT NULL, checksum TEXT NOT NULL,"
                      "applied_at_us INTEGER NOT NULL)");
    if (!rc.ok())
        return Result<int>::Err(rc.error());

    auto stmt = db.prepare("SELECT COALESCE(MAX(version), 0) FROM schema_migrations");
    if (!stmt.ok())
        return Result<int>::Err(stmt.error());
    auto step = stmt.value().step();
    if (!step.ok() || step.value() != Stmt::Step::Row) {
        return Result<int>::Err(Error::make(ErrorCode::DbMigration, "cannot read schema version"));
    }
    int version = static_cast<int>(stmt.value().column_int64(0));
    return Result<int>::Ok(version);
}

Result<int> migrate(Db& db) {
    auto version_rc = current_version(db);
    if (!version_rc.ok())
        return version_rc;
    int version = version_rc.value();

    for (size_t i = 0; i < embedded_migrations::kMigrationCount; ++i) {
        const auto& m = embedded_migrations::kMigrations[i];
        int migration_version = static_cast<int>(i) + 1;
        if (migration_version <= version) {
            // Verify the applied migration was not modified on disk.
            auto check = db.prepare("SELECT checksum FROM schema_migrations WHERE version = ?");
            if (!check.ok())
                return Result<int>::Err(check.error());
            check.value().bind_int64(1, migration_version);
            auto step = check.value().step();
            if (step.ok() && step.value() == Stmt::Step::Row) {
                if (check.value().column_text(0) != m.sha256) {
                    return Result<int>::Err(Error::make(ErrorCode::DbMigration,
                                                        "applied migration checksum changed: " + std::string(m.name))
                                                .ctx("version", std::to_string(migration_version)));
                }
            }
            continue;
        }

        auto txn = Txn::begin(db);
        if (!txn.ok())
            return Result<int>::Err(txn.error());
        auto exec = txn.value().exec(m.sql); // will be rolled back by ~Txn on failure
        if (!exec.ok()) {
            txn.value().rollback();
            Error err = exec.error();
            err.ctx("migration", std::string(m.name));
            return Result<int>::Err(std::move(err));
        }
        auto ins = txn.value().prepare("INSERT INTO schema_migrations(version, name, checksum, applied_at_us) "
                                       "VALUES(?, ?, ?, ?)");
        if (!ins.ok())
            return Result<int>::Err(ins.error());
        ins.value().bind_int64(1, migration_version);
        ins.value().bind_text(2, m.name);
        ins.value().bind_text(3, m.sha256);
        ins.value().bind_int64(4, now_unix_us());
        auto step = ins.value().step();
        if (!step.ok() || step.value() != Stmt::Step::Done) {
            return Result<int>::Err(
                Error::make(ErrorCode::DbMigration, "cannot record migration: " + std::string(m.name)));
        }
        auto commit = txn.value().commit();
        if (!commit.ok())
            return Result<int>::Err(commit.error());
        version = migration_version;
    }
    return Result<int>::Ok(version);
}

} // namespace storage
} // namespace streamforge
