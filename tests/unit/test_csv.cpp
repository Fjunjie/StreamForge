#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "streamforge/ingest/csv_parser.hpp"

using namespace streamforge;

namespace {
std::vector<RawRecord> read_all(const std::string& text, std::vector<Error>* skips = nullptr) {
    std::istringstream in(text);
    CsvParser parser(in, CsvOptions{});
    std::vector<RawRecord> out;
    while (true) {
        auto next = parser.next();
        if (next.kind == CsvParser::Next::Kind::Eof)
            break;
        if (next.kind == CsvParser::Next::Kind::Record) {
            out.push_back(std::move(next.record));
        } else if (next.kind == CsvParser::Next::Kind::SkipRecord && skips != nullptr) {
            skips->push_back(next.error);
        }
    }
    return out;
}
} // namespace

TEST_CASE("csv basic parsing with reordered columns", "[csv]") {
    auto records = read_all("metric,device_id,value,event_time,unit\n"
                            "temp,dev-01,21.5,2026-08-01T09:15:30Z,C\n");
    REQUIRE(records.size() == 1);
    CHECK(records[0].device_id == "dev-01");
    CHECK(records[0].metric == "temp");
    CHECK(records[0].value == 21.5);
    CHECK(records[0].unit == "C");
    CHECK(records[0].has_value);
    CHECK_FALSE(records[0].value_is_null);
    CHECK(records[0].line_no == 2);
}

TEST_CASE("csv quoted fields with delimiters, doubled quotes and newlines", "[csv]") {
    std::vector<Error> skips;
    auto records = read_all("device_id,metric,event_time,value,unit,tags\n"
                            "dev-01,temp,\"2026-08-01T09:15:30Z\",1,C,\"note=\"\"ok\"\"|k=v\"\n"
                            "dev-01,temp,2026-08-01T09:16:30Z,\"multi\nline\",C,\n",
                            &skips);
    REQUIRE(records.size() == 1);
    CHECK(records[0].value == 1.0);
    CHECK(records[0].event_time_raw == "2026-08-01T09:15:30Z");
    CHECK(records[0].tags.size() == 2);
    CHECK(records[0].tags[0].first == "note");
    CHECK(records[0].tags[0].second == "\"ok\"");
    CHECK(records[0].tags[1].second == "v");
    // The multi-line record assembled correctly but its value is not a number: skipped.
    REQUIRE(skips.size() == 1);
    CHECK(skips[0].code == ErrorCode::FormatBadRecord);
}

TEST_CASE("csv comments, empty lines and BOM", "[csv]") {
    std::string text = "\xEF\xBB\xBF"
                       "device_id,metric,event_time,value,unit\n"
                       "# a comment line\n"
                       "\n"
                       "dev-01,temp,2026-08-01T09:15:30Z,1,C\n"
                       "   \n"
                       "dev-01,temp,2026-08-01T09:16:30Z,2,C\n";
    auto records = read_all(text);
    REQUIRE(records.size() == 2);
    CHECK(records[0].value == 1.0);
    CHECK(records[1].value == 2.0);
}

TEST_CASE("csv semicolon delimiter auto-detected", "[csv]") {
    auto records = read_all("device_id;metric;event_time;value;unit\n"
                            "dev-01;temp;2026-08-01T09:15:30Z;3.5;C\n");
    REQUIRE(records.size() == 1);
    CHECK(records[0].value == 3.5);
}

TEST_CASE("csv null value via empty cell", "[csv]") {
    auto records = read_all("device_id,metric,event_time,value,unit\n"
                            "dev-01,temp,2026-08-01T09:15:30Z,,C\n");
    REQUIRE(records.size() == 1);
    CHECK(records[0].value_is_null);
}

TEST_CASE("csv quality, sequence and tags", "[csv]") {
    auto records = read_all("device_id,metric,event_time,value,unit,quality,sequence,tags\n"
                            "dev-01,temp,2026-08-01T09:15:30Z,10,C,2,18342,line=A|zone=east\n");
    REQUIRE(records.size() == 1);
    CHECK(records[0].quality == 2);
    CHECK(records[0].has_quality);
    CHECK(records[0].sequence == 18342);
    CHECK(records[0].tags[0].first == "line");
    CHECK(records[0].tags[0].second == "A");
}

TEST_CASE("csv missing required header column is fatal", "[csv]") {
    std::istringstream in("device_id,metric,value,unit\n");
    CsvParser parser(in, CsvOptions{});
    auto next = parser.next();
    CHECK(next.kind == CsvParser::Next::Kind::Fatal);
    CHECK(next.error.code == ErrorCode::FormatBadHeader);
}

TEST_CASE("csv column count mismatch skips the record", "[csv]") {
    std::istringstream in("device_id,metric,event_time,value,unit\n"
                          "dev-01,temp,2026-08-01T09:15:30Z,1\n");
    CsvParser parser(in, CsvOptions{});
    auto next = parser.next();
    CHECK(next.kind == CsvParser::Next::Kind::SkipRecord);
    CHECK(next.error.code == ErrorCode::FormatBadRecord);
}

TEST_CASE("csv overlong record is skipped with bounded memory", "[csv]") {
    std::string big(2048, 'x');
    std::string text = "device_id,metric,event_time,value,unit\n" + big + "\n";
    std::istringstream in(text);
    CsvParser parser(in, CsvOptions{Delimiter::Auto, 1024});
    auto first = parser.next(); // header consumed; the big record comes back skipped
    CHECK(first.kind == CsvParser::Next::Kind::SkipRecord);
    CHECK(first.error.code == ErrorCode::FormatLineTooLong);
    auto second = parser.next();
    CHECK(second.kind == CsvParser::Next::Kind::Eof);
}

TEST_CASE("csv unterminated quote at eof", "[csv]") {
    std::istringstream in("device_id,metric,event_time,value,unit\n"
                          "dev-01,temp,2026-08-01T09:15:30Z,\"unterminated,C\n");
    CsvParser parser(in, CsvOptions{});
    auto first = parser.next(); // header consumed; unterminated record reported
    CHECK(first.kind == CsvParser::Next::Kind::SkipRecord);
    CHECK(first.error.code == ErrorCode::FormatUnterminatedQuote);
    CHECK(parser.next().kind == CsvParser::Next::Kind::Eof);
}

TEST_CASE("csv offsets form a contiguous checkpoint trail", "[csv][checkpoint]") {
    std::string text = "device_id,metric,event_time,value,unit\n"
                       "dev-01,temp,2026-08-01T09:15:30Z,1,C\n"
                       "dev-01,temp,2026-08-01T09:16:30Z,2,C\n";
    std::istringstream in(text);
    CsvParser parser(in, CsvOptions{});
    auto r1 = parser.next(); // header consumed transparently; first record returned
    auto offset_after_first = parser.current_offset();
    auto r2 = parser.next();
    auto offset_after_second = parser.current_offset();
    CHECK(r1.kind == CsvParser::Next::Kind::Record);
    CHECK(offset_after_first == static_cast<int64_t>(text.find("dev-01,temp,2026-08-01T09:16:30Z")));
    CHECK(offset_after_second == static_cast<int64_t>(text.size()));
    // Resume flow: fresh parser over the whole file, re-consume the header, then jump to the
    // checkpoint offset — exactly what the pipeline does after a crash.
    std::istringstream resume_stream(text);
    CsvParser resume_parser(resume_stream, CsvOptions{});
    auto header = resume_parser.consume_header();
    REQUIRE(header.kind == CsvParser::Next::Kind::Eof);
    resume_stream.clear();
    resume_stream.seekg(offset_after_first);
    resume_parser.seek_to(offset_after_first);
    auto resumed = resume_parser.next();
    CHECK(resumed.kind == CsvParser::Next::Kind::Record);
    CHECK(resumed.record.value == r2.record.value);
}
