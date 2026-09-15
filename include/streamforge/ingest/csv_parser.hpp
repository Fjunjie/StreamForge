#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "streamforge/core/error.hpp"
#include "streamforge/ingest/line_reader.hpp"
#include "streamforge/ingest/raw_record.hpp"

namespace streamforge {

enum class Delimiter { Auto, Comma, Semicolon };

struct CsvOptions {
    Delimiter delimiter = Delimiter::Auto;
    size_t max_record_bytes = 1024ULL * 1024; // requirement FR-FMT-002
};

// Streaming RFC 4180 CSV parser with a Record/Skip/Fatal/Eof protocol.
// - UTF-8 BOM tolerated; header row mandatory, column order free.
// - Comment lines start with '#' (only when not inside quotes); empty lines ignored.
// - Quoted fields may embed delimiters, doubled quotes and newlines.
class CsvParser {
public:
    struct Next {
        enum class Kind { Eof, Record, SkipRecord, Fatal };
        Kind kind = Kind::Eof;
        RawRecord record;
        Error error;
    };

    CsvParser(std::istream& in, CsvOptions options);

    [[nodiscard]] const std::vector<std::string>& header() const { return header_; }
    [[nodiscard]] const std::vector<std::string>& warnings() const { return warnings_; }
    [[nodiscard]] char delimiter() const { return delimiter_; }
    // Byte offset of the next unread byte; the safe checkpoint position after the last
    // record returned (requirement FR-REC-001).
    [[nodiscard]] int64_t current_offset() const { return offset_; }

    // Reads and validates the header from the current position without returning a record.
    // Used on checkpoint resume, where the safe offset sits past the header. Returns Eof
    // (header consumed or end reached) or Fatal for a bad header.
    Next consume_header();

    // Re-baselines the internal offset/line counters after an external istream seekg to a
    // checkpoint offset.
    void seek_to(int64_t offset);

    Next next();

private:
    struct PhysicalLine {
        std::string text; // without trailing newline; '\r' kept (stripped by caller when appropriate)
        bool truncated = false;
        int64_t bytes_consumed = 0;
    };

    bool read_physical_line(PhysicalLine& out);
    // Splits one physical line on the delimiter, honouring quotes; returns false on error.
    [[nodiscard]] std::vector<std::string> split_line(const std::string& line) const;
    static bool line_is_balanced(const std::string& line);

    CsvOptions options_;
    LineReader reader_;
    char delimiter_ = ',';
    bool delimiter_chosen_ = false;
    bool header_done_ = false;
    bool fatal_ = false;
    std::vector<std::string> header_;
    std::vector<std::string> warnings_;
    int64_t offset_ = 0;  // byte offset of the next unread byte
    int64_t line_no_ = 0; // physical line counter
};

} // namespace streamforge
