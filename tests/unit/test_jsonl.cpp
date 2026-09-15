#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "streamforge/ingest/jsonl_parser.hpp"

using namespace streamforge;

namespace {
std::vector<RawRecord> read_all(const std::string& text, std::vector<Error>* skips = nullptr) {
    std::istringstream in(text);
    JsonlParser parser(in);
    std::vector<RawRecord> out;
    while (true) {
        auto next = parser.next();
        if (next.kind == JsonlParser::Next::Kind::Eof)
            break;
        if (next.kind == JsonlParser::Next::Kind::Record) {
            out.push_back(std::move(next.record));
        } else if (next.kind == JsonlParser::Next::Kind::SkipRecord && skips != nullptr) {
            skips->push_back(next.error);
        }
    }
    return out;
}
} // namespace

TEST_CASE("jsonl basic record", "[jsonl]") {
    auto records =
        read_all(R"({"device_id":"dev-01","metric":"temp","event_time":"2026-08-01T09:15:30Z","value":21.5,"unit":"C"})"
                 "\n");
    REQUIRE(records.size() == 1);
    CHECK(records[0].device_id == "dev-01");
    CHECK(records[0].value == 21.5);
    CHECK(records[0].unit == "C");
    CHECK(records[0].line_no == 1);
}

TEST_CASE("jsonl unix millis event_time, quality, sequence and tags", "[jsonl]") {
    auto records = read_all(
        R"({"device_id":"dev-01","metric":"temp","event_time":1785598530125,"value":null,"unit":"C","quality":1,"sequence":7,"tags":{"line":"A"}})"
        "\n");
    REQUIRE(records.size() == 1);
    CHECK(records[0].event_time_raw == "1785598530125");
    CHECK(records[0].value_is_null);
    CHECK(records[0].quality == 1);
    CHECK(records[0].sequence == 7);
    REQUIRE(records[0].tags.size() == 1);
    CHECK(records[0].tags[0].first == "line");
}

TEST_CASE("jsonl unknown fields preserved in ext_json", "[jsonl]") {
    auto records = read_all(
        R"({"device_id":"dev-01","metric":"temp","event_time":"2026-08-01T09:15:30Z","value":1,"unit":"C","custom":{"nested":[1,2]},"firmware":"1.2"})"
        "\n");
    REQUIRE(records.size() == 1);
    CHECK(records[0].ext_json.find("\"custom\"") != std::string::npos);
    CHECK(records[0].ext_json.find("\"firmware\":\"1.2\"") != std::string::npos);
}

TEST_CASE("jsonl rejects top level arrays and non-objects", "[jsonl]") {
    std::vector<Error> skips;
    read_all("[1,2,3]\n{\"device_id\":\"dev-01\",\"metric\":\"temp\",\"event_time\":"
             "\"2026-08-01T09:15:30Z\",\"value\":1,\"unit\":\"C\"}\n\"text\"\n",
             &skips);
    REQUIRE(skips.size() == 2);
    CHECK(skips[0].code == ErrorCode::FormatBadJson);
    CHECK(skips[1].code == ErrorCode::FormatBadJson);
}

TEST_CASE("jsonl rejects numbers as strings, NaN and Infinity", "[jsonl]") {
    std::vector<Error> skips;
    read_all(R"({"device_id":"dev-01","metric":"temp","event_time":"2026-08-01T09:15:30Z","value":"21.5","unit":"C"})"
             "\n"
             R"({"device_id":"dev-01","metric":"temp","event_time":"2026-08-01T09:15:30Z","value":NaN,"unit":"C"})"
             "\n"
             R"({"device_id":"dev-01","metric":"temp","event_time":"2026-08-01T09:15:30Z","value":Infinity,"unit":"C"})"
             "\n",
             &skips);
    REQUIRE(skips.size() == 3);
    CHECK(skips[0].code == ErrorCode::FormatJsonType);
    CHECK(skips[0].message.find("strings") != std::string::npos);
    // nlohmann's strict parser rejects NaN/Infinity literals as JSON syntax errors.
    CHECK(skips[1].code == ErrorCode::FormatBadJson);
    CHECK(skips[2].code == ErrorCode::FormatBadJson);
}

TEST_CASE("jsonl enforces nesting depth limit", "[jsonl]") {
    std::string deep;
    for (int i = 0; i < 20; ++i)
        deep += "{\"a\":";
    deep += "1";
    for (int i = 0; i < 20; ++i)
        deep += "}";
    std::vector<Error> skips;
    read_all(deep + "\n", &skips);
    REQUIRE(skips.size() == 1);
    CHECK(skips[0].code == ErrorCode::FormatJsonDepth);
}

TEST_CASE("jsonl missing required fields", "[jsonl]") {
    std::vector<Error> skips;
    read_all(R"({"device_id":"dev-01","metric":"temp","value":1,"unit":"C"})"
             "\n", // no event_time
             &skips);
    REQUIRE(skips.size() == 1);
    CHECK(skips[0].code == ErrorCode::FormatBadRecord);
    CHECK(skips[0].message.find("event_time") != std::string::npos);
}

TEST_CASE("jsonl invalid json skipped", "[jsonl]") {
    std::vector<Error> skips;
    read_all("{not json}\n", &skips);
    REQUIRE(skips.size() == 1);
    CHECK(skips[0].code == ErrorCode::FormatBadJson);
}

TEST_CASE("jsonl blank lines ignored and line numbers count all lines", "[jsonl]") {
    auto records = read_all(
        "\n\n" +
        std::string(
            R"({"device_id":"dev-01","metric":"temp","event_time":"2026-08-01T09:15:30Z","value":1,"unit":"C"})") +
        "\n");
    REQUIRE(records.size() == 1);
    CHECK(records[0].line_no == 3);
}
