#include "streamforge/core/error.hpp"

#include <array>

namespace streamforge {
namespace {

struct CodeName {
    ErrorCode code;
    const char* name;
};

constexpr std::array<CodeName, 60> kCodeNames{{
    {ErrorCode::None, "None"},
    {ErrorCode::InvalidArgument, "InvalidArgument"},
    {ErrorCode::InternalError, "InternalError"},
    {ErrorCode::NotImplemented, "NotImplemented"},
    {ErrorCode::ConfigFileOpen, "ConfigFileOpen"},
    {ErrorCode::ConfigParse, "ConfigParse"},
    {ErrorCode::ConfigValidation, "ConfigValidation"},
    {ErrorCode::ConfigSchemaUnsupported, "ConfigSchemaUnsupported"},
    {ErrorCode::IoOpen, "IoOpen"},
    {ErrorCode::IoRead, "IoRead"},
    {ErrorCode::IoWrite, "IoWrite"},
    {ErrorCode::IoRename, "IoRename"},
    {ErrorCode::IoRemove, "IoRemove"},
    {ErrorCode::IoStat, "IoStat"},
    {ErrorCode::PathInvalid, "PathInvalid"},
    {ErrorCode::PathOutsideRoot, "PathOutsideRoot"},
    {ErrorCode::PathCollision, "PathCollision"},
    {ErrorCode::FormatBadHeader, "FormatBadHeader"},
    {ErrorCode::FormatBadRecord, "FormatBadRecord"},
    {ErrorCode::FormatLineTooLong, "FormatLineTooLong"},
    {ErrorCode::FormatBadCsvField, "FormatBadCsvField"},
    {ErrorCode::FormatBadJson, "FormatBadJson"},
    {ErrorCode::FormatJsonDepth, "FormatJsonDepth"},
    {ErrorCode::FormatJsonType, "FormatJsonType"},
    {ErrorCode::FormatUnsupported, "FormatUnsupported"},
    {ErrorCode::FormatTagSyntax, "FormatTagSyntax"},
    {ErrorCode::FormatUnterminatedQuote, "FormatUnterminatedQuote"},
    {ErrorCode::ValidationDeviceUnknown, "ValidationDeviceUnknown"},
    {ErrorCode::ValidationMetricUnknown, "ValidationMetricUnknown"},
    {ErrorCode::ValidationTimeInvalid, "ValidationTimeInvalid"},
    {ErrorCode::ValidationValueInvalid, "ValidationValueInvalid"},
    {ErrorCode::ValidationUnitUnknown, "ValidationUnitUnknown"},
    {ErrorCode::ValidationQualityInvalid, "ValidationQualityInvalid"},
    {ErrorCode::ValidationTagsInvalid, "ValidationTagsInvalid"},
    {ErrorCode::ValidationCharset, "ValidationCharset"},
    {ErrorCode::ValidationErrorRateExceeded, "ValidationErrorRateExceeded"},
    {ErrorCode::DbOpen, "DbOpen"},
    {ErrorCode::DbExec, "DbExec"},
    {ErrorCode::DbPrepare, "DbPrepare"},
    {ErrorCode::DbStep, "DbStep"},
    {ErrorCode::DbBind, "DbBind"},
    {ErrorCode::DbBusy, "DbBusy"},
    {ErrorCode::DbConstraint, "DbConstraint"},
    {ErrorCode::DbCorrupt, "DbCorrupt"},
    {ErrorCode::DbMigration, "DbMigration"},
    {ErrorCode::DbNotFound, "DbNotFound"},
    {ErrorCode::SourceMissing, "SourceMissing"},
    {ErrorCode::SourceChanged, "SourceChanged"},
    {ErrorCode::FileSkippedDuplicate, "FileSkippedDuplicate"},
    {ErrorCode::FileQuarantined, "FileQuarantined"},
    {ErrorCode::Interrupted, "Interrupted"},
    {ErrorCode::ShutdownTimeout, "ShutdownTimeout"},
}};

} // namespace

const char* Error::code_name() const {
    for (const auto& entry : kCodeNames) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "Unknown";
}

} // namespace streamforge
