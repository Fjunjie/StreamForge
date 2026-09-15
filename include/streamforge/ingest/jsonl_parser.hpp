#pragma once

#include <cstddef>
#include <istream>
#include <string>

#include "streamforge/core/error.hpp"
#include "streamforge/ingest/raw_record.hpp"

namespace streamforge {

// Streaming JSON Lines parser (requirement FR-FMT-003).
// One JSON object per line; top-level arrays rejected; nesting depth <= 16 checked before
// parsing so hostile inputs cannot exhaust the stack; numbers-as-strings and NaN/Infinity
// literals are rejected; unknown fields are preserved in RawRecord::ext_json.
class JsonlParser {
public:
    struct Next {
        enum class Kind { Eof, Record, SkipRecord };
        Kind kind = Kind::Eof;
        RawRecord record;
        Error error;
    };

    explicit JsonlParser(std::istream& in, size_t max_line_bytes = 1024ULL * 1024, int max_depth = 16);

    // Byte offset of the next unread byte; the safe checkpoint position.
    [[nodiscard]] int64_t current_offset() const { return offset_; }

    // Re-baselines the internal offset/line counters after an external istream seekg to a
    // checkpoint offset.
    void seek_to(int64_t offset);

    Next next();

private:
    std::istream& in_;
    size_t max_line_bytes_;
    int max_depth_;
    int64_t offset_ = 0;
    int64_t line_no_ = 0;
};

} // namespace streamforge
