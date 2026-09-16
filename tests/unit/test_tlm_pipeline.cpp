#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <vector>

#include "streamforge/ingest/tlm_parser.hpp"
#include "support/test_env.hpp"

using namespace streamforge;

namespace {
struct FrameBuilder {
    std::vector<uint8_t> payload;
    void tlv(uint8_t type, const std::vector<uint8_t>& value) {
        payload.push_back(type);
        payload.push_back(static_cast<uint8_t>(value.size() & 0xFF));
        payload.push_back(static_cast<uint8_t>((value.size() >> 8) & 0xFF));
        payload.insert(payload.end(), value.begin(), value.end());
    }
    void str(uint8_t type, const std::string& s) { tlv(type, std::vector<uint8_t>(s.begin(), s.end())); }
    std::vector<uint8_t> build(uint8_t frame_type = 1, uint8_t flags = 0, uint64_t sequence = 1) {
        std::vector<uint8_t> frame(16);
        tlm_write_frame_header(frame.data(), frame_type, flags, static_cast<uint32_t>(payload.size()), sequence);
        uint32_t crc = tlm_crc32(payload.data(), payload.size());
        uint8_t crc_buf[4];
        tlm_write_u32(crc_buf, crc);
        frame.insert(frame.end(), payload.begin(), payload.end());
        frame.insert(frame.end(), crc_buf, crc_buf + 4);
        return frame;
    }
};

std::vector<uint8_t> file_header(uint64_t created_at_ms) {
    std::vector<uint8_t> h(24);
    std::memcpy(h.data(), "TLM1", 4);
    tlm_write_u16(h.data() + 4, 1);
    tlm_write_u16(h.data() + 6, 24);
    tlm_write_u64(h.data() + 8, created_at_ms);
    tlm_write_u32(h.data() + 16, 0);
    tlm_write_u32(h.data() + 20, tlm_crc32(h.data(), 20));
    return h;
}

// A temp file holding a small TLM stream (header + frames).
struct TlmFile {
    sf_test::TempDir dir;
    std::string path;
    TlmFile(const std::vector<std::vector<uint8_t>>& frames) {
        std::vector<uint8_t> all = file_header(1785598530125ULL);
        for (const auto& f : frames)
            all.insert(all.end(), f.begin(), f.end());
        path = dir.file("input/stream.tlm", std::string(all.begin(), all.end()));
    }
};

FrameBuilder base_frame() {
    FrameBuilder b;
    b.str(0x01, "dev-01");
    b.str(0x02, "temp");
    std::vector<uint8_t> ts(8);
    tlm_write_u64(ts.data(), 1785598530125ULL);
    b.tlv(0x03, ts);
    b.tlv(0x05, {}); // value_null
    b.str(0x06, "C");
    return b;
}
} // namespace

TEST_CASE("tlm parser decodes records and skips metadata frames", "[tlm-parser]") {
    auto hb = base_frame().build(1, 0, 1);
    auto heartbeat = FrameBuilder{}.build(3, 0, 2);
    auto meta = FrameBuilder{}.build(2, 0, 3);
    auto data2 = base_frame().build(1, 0, 4);
    TlmFile file({hb, heartbeat, meta, data2});

    std::ifstream in(file.path, std::ios::binary);
    TlmParser parser(in);
    auto first = parser.next();
    REQUIRE(first.kind == TlmParser::Next::Kind::Record);
    CHECK(first.record.device_id == "dev-01");
    CHECK(first.record.event_time_raw == "1785598530125");
    CHECK(parser.has_last_sequence());
    CHECK(parser.last_sequence() == 1);
    auto second = parser.next();
    REQUIRE(second.kind == TlmParser::Next::Kind::Record);
    CHECK(parser.last_sequence() == 4);
    CHECK(parser.next().kind == TlmParser::Next::Kind::Eof);
    // Checkpoint sits at the end of the stream.
    std::ifstream in2(file.path, std::ios::binary | std::ios::ate);
    CHECK(parser.current_offset() == static_cast<int64_t>(in2.tellg()));
}

TEST_CASE("tlm parser recovers after corrupt frames and marks data suspicious", "[tlm-parser]") {
    auto good1 = base_frame().build(1, 0, 1);
    auto good2 = base_frame().build(1, 0, 2);
    std::vector<uint8_t> corrupted = base_frame().build(1, 0, 3);
    corrupted[corrupted.size() - 2] ^= 0xFF; // break the payload CRC
    std::vector<uint8_t> garbage{'g', 'a', 'r', 'b', 'a', 'g', 'e', 0x00, 0x11};

    std::vector<uint8_t> all = file_header(1);
    all.insert(all.end(), good1.begin(), good1.end());
    all.insert(all.end(), garbage.begin(), garbage.end());
    all.insert(all.end(), corrupted.begin(), corrupted.end());
    all.insert(all.end(), good2.begin(), good2.end());
    sf_test::TempDir dir;
    auto path = dir.file("input/corrupt.tlm", std::string(all.begin(), all.end()));

    std::ifstream in(path, std::ios::binary);
    TlmParser parser(in);
    auto r1 = parser.next();
    REQUIRE(r1.kind == TlmParser::Next::Kind::Record);
    CHECK(r1.record.quality == 0);
    bool clean_has_resync = false;
    for (const auto& t : r1.record.tags) {
        if (t.first == "resync")
            clean_has_resync = true;
    }
    CHECK_FALSE(clean_has_resync);

    // The corrupt frame is skipped (with or without an explicit skip report) and the
    // resync delivers good2, marked suspicious and tagged resync=true.
    TlmParser::Next r2;
    do {
        r2 = parser.next();
        if (r2.kind == TlmParser::Next::Kind::Eof)
            FAIL("stream ended before recovery");
    } while (r2.kind == TlmParser::Next::Kind::SkipRecord);
    REQUIRE(r2.kind == TlmParser::Next::Kind::Record);
    CHECK(parser.last_sequence() == 2);
    CHECK(r2.record.quality == 1); // forced suspicious (FR-FMT-004)
    bool has_resync_tag = false;
    for (const auto& t : r2.record.tags) {
        if (t.first == "resync")
            has_resync_tag = true;
    }
    CHECK(has_resync_tag);
    CHECK(parser.next().kind == TlmParser::Next::Kind::Eof);
}

TEST_CASE("tlm parser reports truncated trailing frames", "[tlm-parser]") {
    auto good = base_frame().build(1, 0, 1);
    std::vector<uint8_t> all = file_header(1);
    all.insert(all.end(), good.begin(), good.end());
    all.push_back(0x5A); // sync byte without a complete frame
    all.push_back(0xA5);
    all.push_back(0x01);
    sf_test::TempDir dir;
    auto path = dir.file("input/trunc.tlm", std::string(all.begin(), all.end()));

    std::ifstream in(path, std::ios::binary);
    TlmParser parser(in);
    REQUIRE(parser.next().kind == TlmParser::Next::Kind::Record);
    auto last = parser.next();
    CHECK(last.kind == TlmParser::Next::Kind::SkipRecord);
    CHECK(parser.next().kind == TlmParser::Next::Kind::Eof);
}
