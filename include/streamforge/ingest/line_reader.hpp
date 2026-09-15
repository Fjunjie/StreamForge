#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>

namespace streamforge {

// Reads physical lines from a stream with a hard length cap. Overlong lines are reported
// (truncated=true) while the remainder of the physical line is drained, so memory stays
// bounded regardless of input (requirement 10.3).
class LineReader {
public:
    explicit LineReader(std::istream& in, size_t max_line_bytes) : in_(in), max_line_bytes_(max_line_bytes) {}

    struct Line {
        std::string text;
        bool truncated = false;
        int64_t bytes_consumed = 0; // including the trailing newline when present
    };

    // Returns false at end of stream with nothing read.
    bool next(Line& out);

private:
    std::istream& in_;
    size_t max_line_bytes_;
};

} // namespace streamforge
