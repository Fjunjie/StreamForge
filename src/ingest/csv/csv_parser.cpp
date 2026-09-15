#include "streamforge/ingest/csv_parser.hpp"

#include <cerrno>
#include <cstdlib>
#include <map>

#include "streamforge/core/fs_util.hpp"
#include "streamforge/ingest/line_reader.hpp"

namespace streamforge {

CsvParser::CsvParser(std::istream& in, CsvOptions options) : options_(options), reader_(in, options.max_record_bytes) {}

bool CsvParser::read_physical_line(PhysicalLine& out) {
    LineReader::Line line;
    if (!reader_.next(line))
        return false;
    out.text = std::move(line.text);
    out.truncated = line.truncated;
    out.bytes_consumed = line.bytes_consumed;
    offset_ += line.bytes_consumed;
    ++line_no_;
    return true;
}

bool CsvParser::line_is_balanced(const std::string& line) {
    int quotes = 0;
    for (char c : line) {
        if (c == '"')
            ++quotes;
    }
    return quotes % 2 == 0;
}

std::vector<std::string> CsvParser::split_line(const std::string& line) const {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current += '"';
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                current += c;
            }
        } else if (c == '"') {
            in_quotes = true;
        } else if (c == delimiter_) {
            fields.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    fields.push_back(std::move(current));
    return fields;
}

namespace {

// Strips the UTF-8 BOM (first physical line only) and a trailing CR.
std::string clean_line(const std::string& raw, int64_t line_no, int64_t& bom_bytes) {
    std::string text = raw;
    if (line_no == 1 && text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
        bom_bytes = 3;
    }
    if (!text.empty() && text.back() == '\r')
        text.pop_back();
    return text;
}

} // namespace

void CsvParser::seek_to(int64_t offset) {
    offset_ = offset;
    line_no_ = 0; // line numbers restart relative to the resume point
}

CsvParser::Next CsvParser::consume_header() {
    Next out;
    while (true) {
        PhysicalLine line;
        if (!read_physical_line(line)) {
            out.kind = Next::Kind::Eof;
            return out;
        }
        int64_t bom_bytes = 0;
        std::string text = clean_line(line.text, line_no_, bom_bytes);

        if (text.empty())
            continue; // empty line: ignore
        if (!delimiter_chosen_) {
            if (text[0] == '#')
                continue; // comment before header
            size_t commas = 0, semicolons = 0;
            bool in_quotes = false;
            for (char c : text) {
                if (c == '"')
                    in_quotes = !in_quotes;
                if (in_quotes)
                    continue;
                if (c == ',')
                    ++commas;
                if (c == ';')
                    ++semicolons;
            }
            delimiter_ = (options_.delimiter == Delimiter::Semicolon ||
                          (options_.delimiter == Delimiter::Auto && semicolons > commas))
                             ? ';'
                             : ',';
            delimiter_chosen_ = true;
        }
        if (text[0] == '#')
            continue; // comment before header

        header_ = split_line(text);
        for (const auto& h : header_) {
            if (h.empty()) {
                fatal_ = true;
                out.kind = Next::Kind::Fatal;
                out.error = Error::make(ErrorCode::FormatBadHeader, "empty header column name")
                                .ctx("line", std::to_string(line_no_));
                return out;
            }
        }
        static const std::vector<std::string> kRequired = {"device_id", "metric", "event_time", "value", "unit"};
        for (const auto& req : kRequired) {
            bool found = false;
            for (const auto& h : header_) {
                if (h == req)
                    found = true;
            }
            if (!found) {
                fatal_ = true;
                out.kind = Next::Kind::Fatal;
                out.error = Error::make(ErrorCode::FormatBadHeader, "missing required header column '" + req + "'")
                                .ctx("line", std::to_string(line_no_));
                return out;
            }
        }
        static const std::vector<std::string> kKnown = {"device_id", "metric",  "event_time", "value",
                                                        "unit",      "quality", "sequence",   "tags"};
        for (const auto& h : header_) {
            bool known = false;
            for (const auto& k : kKnown) {
                if (h == k)
                    known = true;
            }
            if (!known)
                warnings_.push_back("unknown header column '" + h + "'");
        }
        header_done_ = true;
        out.kind = Next::Kind::Eof;
        return out;
    }
}

CsvParser::Next CsvParser::next() {
    Next out;
    if (fatal_) {
        out.kind = Next::Kind::Fatal;
        out.error = Error::make(ErrorCode::FormatBadHeader, "parser already failed");
        return out;
    }

    if (!header_done_) {
        auto h = consume_header();
        if (h.kind == Next::Kind::Fatal)
            return h;
        if (!header_done_) {
            out.kind = Next::Kind::Eof; // end of stream before any header
            return out;
        }
    }

    while (true) {
        PhysicalLine line;
        if (!read_physical_line(line)) {
            out.kind = Next::Kind::Eof;
            return out;
        }
        int64_t record_start_offset = offset_ - line.bytes_consumed;
        int64_t record_start_line = line_no_;

        int64_t bom_bytes = 0;
        std::string text = clean_line(line.text, line_no_, bom_bytes);
        if (text.empty())
            continue; // empty line: ignore
        if (text[0] == '#')
            continue; // comment line (non-quoted '#' lines)

        // Assemble quoted continuation lines.
        std::string record = text;
        int64_t record_bytes = line.bytes_consumed - bom_bytes;
        bool truncated_record = line.truncated;
        while (!line_is_balanced(record)) {
            if (record_bytes > static_cast<int64_t>(options_.max_record_bytes)) {
                out.kind = Next::Kind::SkipRecord;
                out.error = Error::make(ErrorCode::FormatLineTooLong, "CSV record exceeds size limit")
                                .ctx("line", std::to_string(record_start_line));
                // Drain the remainder of the overlong record.
                std::string drop;
                while (!line_is_balanced(drop)) {
                    PhysicalLine more;
                    if (!read_physical_line(more)) {
                        out.kind = Next::Kind::SkipRecord;
                        out.error =
                            Error::make(ErrorCode::FormatUnterminatedQuote, "unterminated quoted field at end of file")
                                .ctx("line", std::to_string(record_start_line));
                        return out;
                    }
                    drop = more.text;
                    record_bytes += more.bytes_consumed;
                }
                return out;
            }
            PhysicalLine more;
            if (!read_physical_line(more)) {
                out.kind = Next::Kind::SkipRecord;
                out.error = Error::make(ErrorCode::FormatUnterminatedQuote, "unterminated quoted field at end of file")
                                .ctx("line", std::to_string(record_start_line));
                return out;
            }
            record += "\n";
            record += more.text;
            record_bytes += more.bytes_consumed;
            truncated_record = truncated_record || more.truncated;
        }

        if (truncated_record || record_bytes > static_cast<int64_t>(options_.max_record_bytes)) {
            out.kind = Next::Kind::SkipRecord;
            out.error = Error::make(ErrorCode::FormatLineTooLong, "CSV record exceeds size limit")
                            .ctx("line", std::to_string(record_start_line));
            return out;
        }

        std::vector<std::string> fields = split_line(record);
        if (fields.size() != header_.size()) {
            out.kind = Next::Kind::SkipRecord;
            out.error =
                Error::make(ErrorCode::FormatBadRecord, "column count mismatch: " + std::to_string(fields.size()) +
                                                            " vs " + std::to_string(header_.size()))
                    .ctx("line", std::to_string(record_start_line));
            return out;
        }

        RawRecord rec;
        rec.position = record_start_offset;
        rec.line_no = record_start_line;
        rec.raw_prefix = sanitize_raw(record, 256);

        auto cell = [&](const std::string& name) -> const std::string* {
            for (size_t i = 0; i < header_.size(); ++i) {
                if (header_[i] == name)
                    return &fields[i];
            }
            return nullptr;
        };
        auto cell_str = [&](const std::string& name) -> std::string {
            const std::string* c = cell(name);
            return c ? *c : std::string();
        };

        rec.device_id = cell_str("device_id");
        rec.metric = cell_str("metric");
        rec.event_time_raw = cell_str("event_time");
        rec.unit = cell_str("unit");

        std::string value_cell = cell_str("value");
        if (value_cell.empty()) {
            rec.has_value = true;
            rec.value_is_null = true;
        } else {
            char* endp = nullptr;
            errno = 0;
            double v = std::strtod(value_cell.c_str(), &endp);
            if (endp != value_cell.c_str() + value_cell.size()) {
                out.kind = Next::Kind::SkipRecord;
                out.error = Error::make(ErrorCode::FormatBadRecord, "value is not a number: " + value_cell)
                                .ctx("line", std::to_string(record_start_line));
                return out;
            }
            rec.has_value = true;
            rec.value = v;
        }

        std::string quality_cell = cell_str("quality");
        if (!quality_cell.empty()) {
            char* endp = nullptr;
            long q = std::strtol(quality_cell.c_str(), &endp, 10);
            if (endp != quality_cell.c_str() + quality_cell.size() || q < 0) {
                out.kind = Next::Kind::SkipRecord;
                out.error =
                    Error::make(ErrorCode::FormatBadRecord, "quality is not a non-negative integer: " + quality_cell)
                        .ctx("line", std::to_string(record_start_line));
                return out;
            }
            rec.has_quality = true;
            rec.quality = static_cast<int>(q);
        }

        std::string sequence_cell = cell_str("sequence");
        if (!sequence_cell.empty()) {
            char* endp = nullptr;
            unsigned long long s = std::strtoull(sequence_cell.c_str(), &endp, 10);
            if (endp != sequence_cell.c_str() + sequence_cell.size() || sequence_cell[0] == '-') {
                out.kind = Next::Kind::SkipRecord;
                out.error =
                    Error::make(ErrorCode::FormatBadRecord, "sequence is not an unsigned integer: " + sequence_cell)
                        .ctx("line", std::to_string(record_start_line));
                return out;
            }
            rec.has_sequence = true;
            rec.sequence = s;
        }

        std::string tags_cell = cell_str("tags");
        if (!tags_cell.empty()) {
            size_t start = 0;
            while (start <= tags_cell.size()) {
                size_t pipe = tags_cell.find('|', start);
                std::string item =
                    (pipe == std::string::npos) ? tags_cell.substr(start) : tags_cell.substr(start, pipe - start);
                if (!item.empty()) {
                    size_t eq = item.find('=');
                    if (eq == std::string::npos || eq == 0) {
                        out.kind = Next::Kind::SkipRecord;
                        out.error = Error::make(ErrorCode::FormatTagSyntax, "tag entry must be key=value: " + item)
                                        .ctx("line", std::to_string(record_start_line));
                        return out;
                    }
                    rec.tags.emplace_back(item.substr(0, eq), item.substr(eq + 1));
                }
                if (pipe == std::string::npos)
                    break;
                start = pipe + 1;
            }
        }

        out.kind = Next::Kind::Record;
        out.record = std::move(rec);
        return out;
    }
}

} // namespace streamforge
