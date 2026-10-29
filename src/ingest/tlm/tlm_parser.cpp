#include "streamforge/ingest/tlm_parser.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

#include "streamforge/core/log.hpp"
#include "streamforge/core/types.hpp"
#include "tlmcodec.h"

namespace streamforge {

namespace {
constexpr size_t kFrameHeaderLen = 16;
constexpr size_t kCrcLen = 4;
constexpr size_t kMaxResyncScan = 1024ULL * 1024ULL;
constexpr size_t kInitialBuffer = 64ULL * 1024ULL;
} // namespace

TlmParser::TlmParser(std::istream& in, size_t max_frame_bytes) : in_(in), max_frame_bytes_(max_frame_bytes) {
    // Start with an empty buffer: the header check must read real stream bytes, and the
    // refill path grows the window as needed.
}

void TlmParser::seek_to(int64_t offset) {
    seek_base_ = offset;
    buf_begin_ = 0;
    eof_ = false;
    resynced_ = false;
    // Resume starts past the file header, so header validation must not re-run.
    header_checked_ = true;
    in_.clear();
}

void TlmParser::set_last_sequence(uint64_t sequence) {
    has_last_sequence_ = true;
    last_sequence_ = sequence;
}

TlmParser::Fill TlmParser::refill() {
    if (eof_)
        return Fill::Eof;
    // Compact: move the unconsumed tail to the front; the append point is its end.
    // (Audit #7: reading from the head would overwrite the unconsumed window whenever
    // a frame spans the read boundary — data loss on any stream larger than one window.)
    if (buf_begin_ > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(buf_begin_));
        seek_base_ += static_cast<int64_t>(buf_begin_);
        buf_begin_ = 0;
    }
    const size_t tail = buf_.size(); // unconsumed bytes kept in place
    if (buf_.size() < kInitialBuffer)
        buf_.resize(kInitialBuffer);
    else if (buf_.size() - tail < 4096)
        buf_.resize(buf_.size() * 2); // make room to append after the tail
    in_.read(reinterpret_cast<char*>(buf_.data() + tail), static_cast<std::streamsize>(buf_.size() - tail));
    const auto got = static_cast<size_t>(in_.gcount());
    if (got == 0) {
        eof_ = true;
        return Fill::Eof;
    }
    buf_.resize(tail + got); // keep only real content
    if (in_.eof())
        eof_ = true; // stream exhausted beyond this window
    return Fill::Got;
}

TlmParser::Next TlmParser::next() {
    Next out;

    if (!header_checked_) {
        header_checked_ = true;
        // Validate the 24-byte file header before accepting any frame.
        while (buf_.size() - buf_begin_ < TLM_FILE_HEADER_SIZE) {
            if (refill() == Fill::Eof) {
                out.kind = Next::Kind::Fatal;
                out.error = Error::make(ErrorCode::FormatBadHeader, "TLM file shorter than its header");
                return out;
            }
        }
        tlm_file_header hdr{};
        tlm_status hrc = tlm_decode_file_header(buf_.data() + buf_begin_, buf_.size() - buf_begin_, &hdr);
        if (hrc != TLM_OK) {
            out.kind = Next::Kind::Fatal;
            out.error =
                Error::make(ErrorCode::FormatBadHeader, "TLM file header invalid (code " + std::to_string(hrc) + ")");
            return out;
        }
        buf_begin_ += TLM_FILE_HEADER_SIZE;
    }

    while (true) {
        // Ensure one frame header is buffered.
        while (buf_.size() - buf_begin_ < kFrameHeaderLen) {
            if (refill() == Fill::Eof) {
                if (buf_.size() - buf_begin_ == 0) {
                    out.kind = Next::Kind::Eof;
                    return out;
                }
                out.kind = Next::Kind::SkipRecord;
                out.error = Error::make(ErrorCode::FormatBadRecord, "truncated trailing TLM frame");
                // Drop the window so the next call reports EOF instead of looping.
                seek_base_ += static_cast<int64_t>(buf_.size() - buf_begin_);
                buf_begin_ = buf_.size();
                return out;
            }
        }

        const uint8_t* base = buf_.data() + buf_begin_;
        size_t avail = buf_.size() - buf_begin_;

        // Resync when the sync word is missing.
        if (!(base[0] == 0x5A && base[1] == 0xA5)) {
            size_t skipped = 0;
            tlm_status rc = tlm_find_sync(base, avail, kMaxResyncScan, &skipped);
            if (rc != TLM_OK) {
                // Nothing in the window: drop it and pull more data.
                seek_base_ += static_cast<int64_t>(avail);
                buf_begin_ = 0;
                buf_.clear();
                buf_.resize(kInitialBuffer);
                if (refill() == Fill::Eof) {
                    out.kind = Next::Kind::Eof;
                    return out;
                }
                continue;
            }
            seek_base_ += static_cast<int64_t>(skipped);
            buf_begin_ += skipped;
            resynced_ = true;
            SPDLOG_LOGGER_WARN(logger("ingest"), "TLM resync: skipped {} garbage bytes", skipped);
            base = buf_.data() + buf_begin_;
            avail = buf_.size() - buf_begin_;
            if (avail < kFrameHeaderLen)
                continue;
        }

        // Read the payload size from the header, then ensure the whole frame is buffered.
        auto read_payload_size = [&base]() {
            return static_cast<uint32_t>(base[4]) | (static_cast<uint32_t>(base[5]) << 8) |
                   (static_cast<uint32_t>(base[6]) << 16) | (static_cast<uint32_t>(base[7]) << 24);
        };
        uint32_t payload_size = read_payload_size();
        size_t frame_len = kFrameHeaderLen + static_cast<size_t>(payload_size) + kCrcLen;
        if (frame_len > max_frame_bytes_) {
            out.kind = Next::Kind::SkipRecord;
            out.error = Error::make(ErrorCode::FormatBadRecord, "TLM frame exceeds size limit");
            // Skip the header and resync from the following byte.
            seek_base_ += static_cast<int64_t>(kFrameHeaderLen);
            buf_begin_ += kFrameHeaderLen;
            resynced_ = true;
            return out;
        }
        while (avail < frame_len) {
            if (refill() == Fill::Eof) {
                out.kind = Next::Kind::SkipRecord;
                out.error = Error::make(ErrorCode::FormatBadRecord, "truncated TLM frame");
                // Drop the window so the next call reports EOF instead of looping on the
                // same truncated frame.
                seek_base_ += static_cast<int64_t>(buf_.size() - buf_begin_);
                buf_begin_ = buf_.size();
                return out;
            }
            base = buf_.data() + buf_begin_;
            avail = buf_.size() - buf_begin_;
            payload_size = read_payload_size();
            frame_len = kFrameHeaderLen + static_cast<size_t>(payload_size) + kCrcLen;
            if (frame_len > max_frame_bytes_) {
                out.kind = Next::Kind::SkipRecord;
                out.error = Error::make(ErrorCode::FormatBadRecord, "TLM frame exceeds size limit");
                seek_base_ += static_cast<int64_t>(kFrameHeaderLen);
                buf_begin_ += kFrameHeaderLen;
                resynced_ = true;
                return out;
            }
        }

        const int64_t frame_offset = current_offset();
        tlm_parser_state st{has_last_sequence_ ? 1 : 0, last_sequence_, resynced_ ? 1 : 0};
        tlm_record record;
        size_t consumed = 0;
        tlm_status rc = tlm_parse_frame(base, avail, &st, &info_, &consumed, &record);
        has_last_sequence_ = st.has_last_sequence != 0;
        last_sequence_ = st.last_sequence;
        resynced_ = st.suspicious != 0;

        if (rc == TLM_ERR_BAD_SYNC) {
            // Defensive: the codec re-checked the sync word.
            seek_base_ += 1;
            buf_begin_ += 1;
            continue;
        }

        buf_begin_ += consumed; // checkpoint position: end of the returned frame

        if (rc == TLM_ERR_SEQUENCE_REGRESSION) {
            out.kind = Next::Kind::SkipRecord;
            out.error = Error::make(ErrorCode::FormatBadRecord, "TLM frame sequence regression")
                            .ctx("sequence", std::to_string(info_.sequence));
            return out;
        }
        if (rc != TLM_OK) {
            // Payload CRC / TLV / compression errors: skip this frame and resync so a
            // corrupt frame can never yield records.
            out.kind = Next::Kind::SkipRecord;
            out.error = Error::make(ErrorCode::FormatBadRecord, "TLM frame error code " + std::to_string(rc));
            resynced_ = true;
            return out;
        }

        if (info_.frame_type != TLM_FRAME_DATA) {
            // Device metadata / heartbeat / unknown frame types: stream on, no record.
            continue;
        }

        out.kind = Next::Kind::Record;
        out.record.device_id = record.device_id;
        out.record.metric = record.metric;
        out.record.event_time_raw = std::to_string(record.event_time_ms); // unix millis
        out.record.has_value = true;
        out.record.value_is_null = record.value_is_null != 0;
        out.record.value = record.value;
        out.record.unit = record.unit;
        out.record.has_quality = record.has_quality != 0;
        out.record.quality = record.quality;
        out.record.has_sequence = record.has_sequence != 0;
        out.record.sequence = record.sequence;
        for (uint16_t i = 0; i < record.tag_count; ++i) {
            out.record.tags.emplace_back(record.tags[i].key, record.tags[i].value);
        }
        if (record.suspicious != 0) {
            // Post-resync data is marked suspicious (requirement FR-FMT-004).
            out.record.has_quality = true;
            if (out.record.quality < static_cast<int>(Quality::Suspicious)) {
                out.record.quality = static_cast<int>(Quality::Suspicious);
            }
            out.record.tags.emplace_back("resync", "true");
        }
        out.record.position = frame_offset;
        return out;
    }
}

} // namespace streamforge
