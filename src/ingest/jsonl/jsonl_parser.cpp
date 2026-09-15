#include "streamforge/ingest/jsonl_parser.hpp"

#include <cmath>

#include <nlohmann/json.hpp>

#include "streamforge/core/fs_util.hpp"
#include "streamforge/ingest/line_reader.hpp"

namespace streamforge {

using nlohmann::json;

namespace {

// Depth scan on the raw text before json::parse so that hostile deep nesting cannot
// overflow the parser's own recursion.
bool raw_depth_within_limit(const std::string& s, int max_depth, int& worst_depth) {
    int depth = 0;
    worst_depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (char c : s) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
            if (depth > worst_depth)
                worst_depth = depth;
            if (depth > max_depth)
                return false;
        } else if (c == '}' || c == ']') {
            if (depth > 0)
                --depth;
        }
    }
    return true;
}

bool json_depth_within_limit(const json& j, int depth, int max_depth) {
    if (depth > max_depth)
        return false;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!json_depth_within_limit(it.value(), depth + 1, max_depth))
                return false;
        }
    } else if (j.is_array()) {
        for (const auto& item : j) {
            if (!json_depth_within_limit(item, depth + 1, max_depth))
                return false;
        }
    }
    return true;
}

} // namespace

JsonlParser::JsonlParser(std::istream& in, size_t max_line_bytes, int max_depth)
    : in_(in), max_line_bytes_(max_line_bytes), max_depth_(max_depth) {}

void JsonlParser::seek_to(int64_t offset) {
    offset_ = offset;
    line_no_ = 0; // line numbers restart relative to the resume point
}

JsonlParser::Next JsonlParser::next() {
    Next out;
    LineReader reader(in_, max_line_bytes_);
    while (true) {
        LineReader::Line line;
        if (!reader.next(line)) {
            out.kind = Next::Kind::Eof;
            return out;
        }
        int64_t record_line = ++line_no_;
        int64_t record_offset = offset_;
        offset_ += line.bytes_consumed;

        if (line.truncated) {
            out.kind = Next::Kind::SkipRecord;
            out.error = Error::make(ErrorCode::FormatLineTooLong, "JSON line exceeds size limit")
                            .ctx("line", std::to_string(record_line));
            return out;
        }

        // Trim ASCII whitespace.
        size_t b = 0, e = line.text.size();
        while (b < e && (line.text[b] == ' ' || line.text[b] == '\t' || line.text[b] == '\r'))
            ++b;
        while (e > b && (line.text[e - 1] == ' ' || line.text[e - 1] == '\t' || line.text[e - 1] == '\r')) {
            --e;
        }
        std::string text = line.text.substr(b, e - b);
        if (text.empty())
            continue; // blank line: skip silently

        int worst = 0;
        if (!raw_depth_within_limit(text, max_depth_, worst)) {
            out.kind = Next::Kind::SkipRecord;
            out.error =
                Error::make(ErrorCode::FormatJsonDepth, "JSON nesting depth exceeds " + std::to_string(max_depth_))
                    .ctx("line", std::to_string(record_line));
            return out;
        }

        json parsed;
        try {
            parsed = json::parse(text);
        } catch (const json::parse_error& err) {
            out.kind = Next::Kind::SkipRecord;
            out.error = Error::make(ErrorCode::FormatBadJson, std::string("invalid JSON: ") + err.what())
                            .ctx("line", std::to_string(record_line));
            return out;
        }
        if (!parsed.is_object()) {
            out.kind = Next::Kind::SkipRecord;
            out.error = Error::make(ErrorCode::FormatBadJson, "top level must be a JSON object")
                            .ctx("line", std::to_string(record_line));
            return out;
        }
        if (!json_depth_within_limit(parsed, 1, max_depth_)) {
            out.kind = Next::Kind::SkipRecord;
            out.error =
                Error::make(ErrorCode::FormatJsonDepth, "JSON nesting depth exceeds " + std::to_string(max_depth_))
                    .ctx("line", std::to_string(record_line));
            return out;
        }

        RawRecord rec;
        rec.position = record_offset;
        rec.line_no = record_line;
        rec.raw_prefix = sanitize_raw(text, 256);

        static const char* kStandard[] = {"device_id", "metric",  "event_time", "value",
                                          "unit",      "quality", "sequence",   "tags"};
        json ext = json::object();
        bool has_ext = false;
        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
            bool standard = false;
            for (const char* k : kStandard) {
                if (it.key() == k)
                    standard = true;
            }
            if (!standard) {
                ext[it.key()] = it.value();
                has_ext = true;
            }
        }
        if (has_ext)
            rec.ext_json = ext.dump();

        auto missing = [&](const char* field) {
            out.kind = Next::Kind::SkipRecord;
            out.error = Error::make(ErrorCode::FormatBadRecord, std::string("missing required field '") + field + "'")
                            .ctx("line", std::to_string(record_line));
        };
        auto type_error = [&](const std::string& what) {
            out.kind = Next::Kind::SkipRecord;
            out.error = Error::make(ErrorCode::FormatJsonType, what).ctx("line", std::to_string(record_line));
        };

        // device_id / metric / unit: strings.
        auto get_string = [&](const char* field, std::string& dest) -> bool {
            auto it = parsed.find(field);
            if (it == parsed.end() || it->is_null()) {
                if (it == parsed.end())
                    missing(field);
                else
                    type_error(std::string("field '") + field + "' must not be null");
                return false;
            }
            if (!it->is_string()) {
                type_error(std::string("field '") + field + "' must be a string");
                return false;
            }
            dest = it->get<std::string>();
            return true;
        };
        if (!get_string("device_id", rec.device_id))
            return out;
        if (!get_string("metric", rec.metric))
            return out;
        if (!get_string("unit", rec.unit))
            return out;

        // event_time: string (ISO 8601) or integer (Unix milliseconds).
        {
            auto it = parsed.find("event_time");
            if (it == parsed.end()) {
                missing("event_time");
                return out;
            }
            if (it->is_string()) {
                rec.event_time_raw = it->get<std::string>();
            } else if (it->is_number_integer()) {
                rec.event_time_raw = std::to_string(it->get<int64_t>());
            } else {
                type_error("field 'event_time' must be a string or integer");
                return out;
            }
        }

        // value: number (finite) or null. Strings are rejected (FR-FMT-003).
        {
            auto it = parsed.find("value");
            if (it == parsed.end()) {
                missing("value");
                return out;
            }
            if (it->is_null()) {
                rec.has_value = true;
                rec.value_is_null = true;
            } else if (it->is_number_float() || it->is_number_integer() || it->is_number_unsigned()) {
                double v = it->get<double>();
                if (!std::isfinite(v)) {
                    type_error("field 'value' must not be NaN or Infinity");
                    return out;
                }
                rec.has_value = true;
                rec.value = v;
            } else {
                type_error(it->is_string() ? "numbers must not be JSON strings (field 'value')"
                                           : "field 'value' must be a number or null");
                return out;
            }
        }

        // quality: integer.
        {
            auto it = parsed.find("quality");
            if (it != parsed.end() && !it->is_null()) {
                if (!it->is_number_integer()) {
                    type_error("field 'quality' must be an integer");
                    return out;
                }
                rec.has_quality = true;
                rec.quality = it->get<int>();
            }
        }

        // sequence: unsigned integer.
        {
            auto it = parsed.find("sequence");
            if (it != parsed.end() && !it->is_null()) {
                if (it->is_number_unsigned()) {
                    rec.has_sequence = true;
                    rec.sequence = it->get<uint64_t>();
                } else if (it->is_number_integer()) {
                    type_error("field 'sequence' must not be negative");
                    return out;
                } else {
                    type_error("field 'sequence' must be an unsigned integer");
                    return out;
                }
            }
        }

        // tags: object of string -> string.
        {
            auto it = parsed.find("tags");
            if (it != parsed.end() && !it->is_null()) {
                if (!it->is_object()) {
                    type_error("field 'tags' must be an object");
                    return out;
                }
                for (auto t = it->begin(); t != it->end(); ++t) {
                    if (!t.value().is_string()) {
                        type_error("tag values must be strings (key '" + t.key() + "')");
                        return out;
                    }
                    rec.tags.emplace_back(t.key(), t.value().get<std::string>());
                }
            }
        }

        out.kind = Next::Kind::Record;
        out.record = std::move(rec);
        return out;
    }
}

} // namespace streamforge
