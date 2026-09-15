#include "streamforge/storage/db.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <sqlite3.h>

#include "streamforge/core/error.hpp"

namespace streamforge {
namespace storage {

Db::~Db() {
    if (db_ != nullptr) {
        sqlite3_close_v2(db_);
    }
}

Db::Db(Db&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

Db& Db::operator=(Db&& other) noexcept {
    if (this != &other) {
        if (db_ != nullptr)
            sqlite3_close_v2(db_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

Result<Db> Db::open(const std::string& path, bool readonly) {
    Db db;
    int flags = readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    flags |= SQLITE_OPEN_FULLMUTEX;
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(path.c_str(), &raw, flags, nullptr) != SQLITE_OK) {
        std::string msg = raw != nullptr ? sqlite3_errmsg(raw) : "unknown sqlite error";
        if (raw != nullptr)
            sqlite3_close_v2(raw);
        return Result<Db>::Err(Error::make(ErrorCode::DbOpen, "cannot open database: " + msg).ctx("path", path));
    }
    db.db_ = raw;
    if (!readonly) {
        auto rc = db.exec("PRAGMA journal_mode=WAL;"
                          "PRAGMA foreign_keys=ON;"
                          "PRAGMA synchronous=NORMAL;"
                          "PRAGMA busy_timeout=2000;"
                          "PRAGMA temp_store=MEMORY;");
        if (!rc.ok()) {
            return Result<Db>::Err(rc.error());
        }
    }
    return {std::move(db)};
}

std::string Db::last_error() const {
    return db_ != nullptr ? sqlite3_errmsg(db_) : "database not open";
}

Result<void> Db::exec(const std::string& sql) {
    if (db_ == nullptr) {
        return Result<void>::Err(Error::make(ErrorCode::DbOpen, "database not open"));
    }
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg != nullptr ? errmsg : sqlite3_errmsg(db_);
        sqlite3_free(errmsg);
        ErrorCode code = ErrorCode::DbExec;
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
            code = ErrorCode::DbBusy;
        if (rc == SQLITE_CORRUPT)
            code = ErrorCode::DbCorrupt;
        if (rc == SQLITE_CONSTRAINT)
            code = ErrorCode::DbConstraint;
        return Result<void>::Err(Error::make(code, "sql exec failed: " + msg).ctx("sql", sql));
    }
    return Result<void>::Ok();
}

int64_t Db::changes() const {
    return db_ != nullptr ? sqlite3_changes(db_) : 0;
}

Result<Stmt> Db::prepare(const std::string& sql) {
    if (db_ == nullptr) {
        return Result<Stmt>::Err(Error::make(ErrorCode::DbOpen, "database not open"));
    }
    Stmt stmt;
    stmt.db_ = db_;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt.stmt_, nullptr) != SQLITE_OK) {
        return Result<Stmt>::Err(Error::make(ErrorCode::DbPrepare, "prepare failed: " + last_error()).ctx("sql", sql));
    }
    return {std::move(stmt)};
}

Stmt::~Stmt() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
}

Stmt::Stmt(Stmt&& other) noexcept : stmt_(other.stmt_), db_(other.db_) {
    other.stmt_ = nullptr;
    other.db_ = nullptr;
}

Stmt& Stmt::operator=(Stmt&& other) noexcept {
    if (this != &other) {
        if (stmt_ != nullptr)
            sqlite3_finalize(stmt_);
        stmt_ = other.stmt_;
        db_ = other.db_;
        other.stmt_ = nullptr;
        other.db_ = nullptr;
    }
    return *this;
}

Result<void> Stmt::bind_int64(int idx, int64_t value) {
    if (sqlite3_bind_int64(stmt_, idx, value) != SQLITE_OK) {
        return Result<void>::Err(
            Error::make(ErrorCode::DbBind, "bind_int64 failed: " + std::string(sqlite3_errmsg(db_))));
    }
    return Result<void>::Ok();
}

Result<void> Stmt::bind_double(int idx, double value) {
    if (sqlite3_bind_double(stmt_, idx, value) != SQLITE_OK) {
        return Result<void>::Err(
            Error::make(ErrorCode::DbBind, "bind_double failed: " + std::string(sqlite3_errmsg(db_))));
    }
    return Result<void>::Ok();
}

Result<void> Stmt::bind_text(int idx, const std::string& value) {
    if (sqlite3_bind_text(stmt_, idx, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        return Result<void>::Err(
            Error::make(ErrorCode::DbBind, "bind_text failed: " + std::string(sqlite3_errmsg(db_))));
    }
    return Result<void>::Ok();
}

Result<void> Stmt::bind_null(int idx) {
    if (sqlite3_bind_null(stmt_, idx) != SQLITE_OK) {
        return Result<void>::Err(
            Error::make(ErrorCode::DbBind, "bind_null failed: " + std::string(sqlite3_errmsg(db_))));
    }
    return Result<void>::Ok();
}

Result<Stmt::Step> Stmt::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW)
        return Result<Step>::Ok(Step::Row);
    if (rc == SQLITE_DONE)
        return Result<Step>::Ok(Step::Done);
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
        return Result<Step>::Ok(Step::Busy);
    return Result<Step>::Err(Error::make(rc == SQLITE_CORRUPT ? ErrorCode::DbCorrupt : ErrorCode::DbStep,
                                         "step failed: " + std::string(sqlite3_errmsg(db_))));
}

Result<void> Stmt::reset() {
    sqlite3_clear_bindings(stmt_);
    if (sqlite3_reset(stmt_) != SQLITE_OK) {
        return Result<void>::Err(Error::make(ErrorCode::DbStep, "reset failed: " + std::string(sqlite3_errmsg(db_))));
    }
    return Result<void>::Ok();
}

int64_t Stmt::column_int64(int col) const {
    return sqlite3_column_int64(stmt_, col);
}

double Stmt::column_double(int col) const {
    return sqlite3_column_double(stmt_, col);
}

bool Stmt::column_is_null(int col) const {
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

std::string Stmt::column_text(int col) const {
    const unsigned char* text = sqlite3_column_text(stmt_, col);
    if (text == nullptr)
        return {};
    int len = sqlite3_column_bytes(stmt_, col);
    return {reinterpret_cast<const char*>(text), static_cast<size_t>(len)};
}

Result<Txn> Txn::begin(Db& db) {
    Txn txn;
    txn.db_ = &db;
    // Bounded exponential backoff on busy (requirement FR-DB-003).
    int64_t delay_ms = 25;
    for (int attempt = 0; attempt < 8; ++attempt) {
        int rc = sqlite3_exec(db.handle(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        if (rc == SQLITE_OK) {
            txn.done_ = false;
            return {std::move(txn)};
        }
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
            return Result<Txn>::Err(Error::make(ErrorCode::DbExec, "BEGIN IMMEDIATE failed: " + db.last_error()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        delay_ms = std::min<int64_t>(delay_ms * 2, 500);
    }
    return Result<Txn>::Err(Error::make(ErrorCode::DbBusy, "database stayed busy after retries (BEGIN IMMEDIATE)"));
}

Txn::~Txn() {
    if (!done_ && db_ != nullptr) {
        sqlite3_exec(db_->handle(), "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

Txn::Txn(Txn&& other) noexcept : db_(other.db_), done_(other.done_) {
    other.db_ = nullptr;
    other.done_ = true;
}

Txn& Txn::operator=(Txn&& other) noexcept {
    if (this != &other) {
        db_ = other.db_;
        done_ = other.done_;
        other.db_ = nullptr;
        other.done_ = true;
    }
    return *this;
}

Result<void> Txn::commit() {
    if (done_ || db_ == nullptr) {
        return Result<void>::Err(Error::make(ErrorCode::DbExec, "transaction already finished"));
    }
    int rc = sqlite3_exec(db_->handle(), "COMMIT", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        // COMMIT failed (e.g. SQLITE_BUSY): the transaction is still open. Leave done_ false
        // so the destructor rolls back instead of leaving the connection stuck inside an
        // open transaction, which would break every subsequent write.
        return Result<void>::Err(Error::make(ErrorCode::DbExec, "COMMIT failed: " + db_->last_error()));
    }
    done_ = true;
    return Result<void>::Ok();
}

void Txn::rollback() {
    if (done_ || db_ == nullptr)
        return;
    sqlite3_exec(db_->handle(), "ROLLBACK", nullptr, nullptr, nullptr);
    done_ = true;
}

Result<void> Txn::exec(const std::string& sql) {
    return db_->exec(sql);
}

Result<Stmt> Txn::prepare(const std::string& sql) {
    return db_->prepare(sql);
}

} // namespace storage
} // namespace streamforge
