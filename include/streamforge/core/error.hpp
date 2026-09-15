#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace streamforge {

// Unified error codes for the whole project (requirement 10.4).
// Values are stable; only append new entries.
enum class ErrorCode : int {
    None = 0,
    // Generic.
    InvalidArgument = 1000,
    InternalError = 1001,
    NotImplemented = 1002,
    // Configuration.
    ConfigFileOpen = 2000,
    ConfigParse = 2001,
    ConfigValidation = 2002,
    ConfigSchemaUnsupported = 2003,
    // Filesystem / paths.
    IoOpen = 3000,
    IoRead = 3001,
    IoWrite = 3002,
    IoRename = 3003,
    IoRemove = 3004,
    IoStat = 3005,
    PathInvalid = 3100,
    PathOutsideRoot = 3101,
    PathCollision = 3102,
    // Input format (structural / severe record errors).
    FormatBadHeader = 4000,
    FormatBadRecord = 4001,
    FormatLineTooLong = 4002,
    FormatBadCsvField = 4003,
    FormatBadJson = 4004,
    FormatJsonDepth = 4005,
    FormatJsonType = 4006,
    FormatUnsupported = 4007,
    FormatTagSyntax = 4008,
    FormatUnterminatedQuote = 4009,
    FormatBadTime = 4010,
    // Business validation (counts toward file error rate).
    ValidationDeviceUnknown = 5000,
    ValidationMetricUnknown = 5001,
    ValidationTimeInvalid = 5002,
    ValidationValueInvalid = 5003,
    ValidationUnitUnknown = 5004,
    ValidationQualityInvalid = 5005,
    ValidationTagsInvalid = 5006,
    ValidationCharset = 5007,
    ValidationErrorRateExceeded = 5008,
    // Storage.
    DbOpen = 6000,
    DbExec = 6001,
    DbPrepare = 6002,
    DbStep = 6003,
    DbBind = 6004,
    DbBusy = 6005,
    DbConstraint = 6006,
    DbCorrupt = 6007,
    DbMigration = 6008,
    DbNotFound = 6009,
    // Ingest lifecycle.
    SourceMissing = 7000,
    SourceChanged = 7001,
    FileSkippedDuplicate = 7002,
    FileQuarantined = 7003,
    // Runtime.
    Interrupted = 8000,
    ShutdownTimeout = 8001,
};

// A single error value with code, message and structured context.
struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;
    std::vector<std::pair<std::string, std::string>> context;

    Error() = default;
    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

    // Attaches a key/value context entry:
    //   return Error::make(code, "msg").ctx("file", path);
    Error& ctx(std::string key, std::string value) & {
        context.emplace_back(std::move(key), std::move(value));
        return *this;
    }
    Error&& ctx(std::string key, std::string value) && {
        context.emplace_back(std::move(key), std::move(value));
        return std::move(*this);
    }

    static Error make(ErrorCode c, std::string msg) { return {c, std::move(msg)}; }

    [[nodiscard]] int code_value() const { return static_cast<int>(code); }
    // Stable, human-readable name of the error code (never nullptr).
    [[nodiscard]] const char* code_name() const;
};

// Result<T> for fallible operations; C++17 has no std::expected.
template <typename T> class Result {
public:
    Result(T value) : v_(std::move(value)) {}
    Result(Error e) : v_(std::move(e)) {}

    [[nodiscard]] bool ok() const { return v_.index() == 0; }
    explicit operator bool() const { return ok(); }

    [[nodiscard]] const Error& error() const { return std::get<1>(v_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(v_); }
    T& value() & { return std::get<0>(v_); }
    T take() { return std::move(std::get<0>(v_)); }

    static Result Ok(T value) { return Result{std::move(value)}; }
    static Result Err(Error e) { return Result{std::move(e)}; }

private:
    std::variant<T, Error> v_;
};

// Void specialization.
template <> class Result<void> {
public:
    Result() : ok_(true) {}
    Result(Error e) : ok_(false), error_(std::move(e)) {}

    [[nodiscard]] bool ok() const { return ok_; }
    explicit operator bool() const { return ok(); }

    [[nodiscard]] const Error& error() const { return error_; }

    static Result Ok() { return Result{}; }
    static Result Err(Error e) { return Result{std::move(e)}; }

private:
    bool ok_;
    Error error_;
};

} // namespace streamforge
