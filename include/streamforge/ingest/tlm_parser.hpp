#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <vector>

#include "streamforge/core/error.hpp"
#include "streamforge/ingest/raw_record.hpp"
#include "tlmcodec.h"

namespace streamforge {

// Streaming TLM parser (requirement FR-FMT-004) over the C11 codec.
//
// Reads frames from a stream and maps data frames onto RawRecord. Corrupt frames trigger
// the protocol resync scan (up to 1 MiB forward for the sync word); records decoded after
// a resync are flagged suspicious (quality forced to 1 unless already 2) and tagged
// resync=true. The sequence regression check is per file, tracked across the stream.
//
// current_offset() is the safe checkpoint position (end of the last frame returned).
// seek_to() re-baselines the offset counter after an external seekg to a frame boundary;
// the caller must also restore the frame sequence via set_last_sequence() (persisted in
// the file checkpoint for TLM files).
class TlmParser {
public:
    struct Next {
        enum class Kind { Eof, Record, SkipRecord, Fatal };
        Kind kind = Kind::Eof;
        RawRecord record;
        Error error;
    };

    explicit TlmParser(std::istream& in, size_t max_frame_bytes = 5ULL * 1024ULL * 1024ULL);

    // Byte offset of the next unread byte; the safe checkpoint position (FR-REC-001).
    [[nodiscard]] int64_t current_offset() const { return seek_base_ + static_cast<int64_t>(buf_begin_); }
    void seek_to(int64_t offset);
    void set_last_sequence(uint64_t sequence);
    // Sequence state for checkpoint persistence (0 before the first accepted frame).
    [[nodiscard]] bool has_last_sequence() const { return has_last_sequence_; }
    [[nodiscard]] uint64_t last_sequence() const { return last_sequence_; }

    Next next();

private:
    enum class Fill { Got, Eof };
    Fill refill();

    std::istream& in_;
    size_t max_frame_bytes_;
    std::vector<uint8_t> buf_;
    size_t buf_begin_ = 0;  // parse cursor inside buf_
    int64_t seek_base_ = 0; // stream offset where buf_[0] sits
    bool eof_ = false;

    bool has_last_sequence_ = false;
    uint64_t last_sequence_ = 0;
    bool resynced_ = false; // set after a resync; stays set for the rest of the stream
                            // (conservative: records after the resync point are quality-marked)
    bool header_checked_ = false;
    tlm_frame_info info_{}; // frame metadata of the frame being decoded
};

} // namespace streamforge
