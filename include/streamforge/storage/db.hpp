#pragma once

#include <cstdint>
#include <string>

#include "streamforge/core/error.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace streamforge {
namespace storage {

class Stmt; // defined below; Db::prepare returns it

// Thin RAII wrapper over sqlite3. Not thread-safe by itself; Store serializes access.
class Db {
public:
    Db() = default;
    ~Db();
    Db(Db&& other) noexcept;
    Db& operator=(Db&& other) noexcept;
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // Opens the database; applies WAL and foreign-key pragmas when writable.
    static Result<Db> open(const std::string& path, bool readonly);

    Result<void> exec(const std::string& sql);
    Result<Stmt> prepare(const std::string& sql);
    [[nodiscard]] sqlite3* handle() const { return db_; }
    [[nodiscard]] int64_t changes() const;
    [[nodiscard]] std::string last_error() const;

private:
    sqlite3* db_ = nullptr;
};

// Prepared statement wrapper. The source Db must outlive it.
class Stmt {
public:
    Stmt() = default;
    ~Stmt();
    Stmt(Stmt&& other) noexcept;
    Stmt& operator=(Stmt&& other) noexcept;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Result<void> bind_int64(int idx, int64_t value);
    Result<void> bind_double(int idx, double value);
    Result<void> bind_text(int idx, const std::string& value);
    Result<void> bind_null(int idx);

    enum class Step { Row, Done, Busy };
    Result<Step> step();

    Result<void> reset();

    [[nodiscard]] int64_t column_int64(int col) const;
    [[nodiscard]] double column_double(int col) const;
    [[nodiscard]] bool column_is_null(int col) const;
    [[nodiscard]] std::string column_text(int col) const;

private:
    friend class Db;
    sqlite3_stmt* stmt_ = nullptr;
    sqlite3* db_ = nullptr;
};

// Transaction with bounded exponential backoff on SQLITE_BUSY for BEGIN IMMEDIATE
// (requirement FR-DB-003): retries up to 8 times, sleeping 25ms doubling up to 500ms.
class Txn {
public:
    static Result<Txn> begin(Db& db);
    ~Txn();
    Txn(Txn&& other) noexcept;
    Txn& operator=(Txn&& other) noexcept;
    Txn(const Txn&) = delete;
    Txn& operator=(const Txn&) = delete;

    Result<void> commit();
    void rollback();

    // Statement execution inside the open transaction.
    Result<void> exec(const std::string& sql);
    Result<Stmt> prepare(const std::string& sql);
    [[nodiscard]] Db& db() const { return *db_; }

private:
    Txn() = default;
    Db* db_ = nullptr;
    bool done_ = true;
};

} // namespace storage
} // namespace streamforge
