#include <catch2/catch_test_macros.hpp>

#include <zlib.h>

#include <cstring>
#include <string>
#include <vector>

#include "tlmcodec.h"

namespace {

// Builds a complete data frame from field values (reference encoder over tlmcodec helpers).
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

tlm_record decode_one(const std::vector<uint8_t>& frame) {
    tlm_parser_state st{};
    tlm_frame_info info{};
    size_t consumed = 0;
    tlm_record rec{};
    tlm_status rc = tlm_parse_frame(frame.data(), frame.size(), &st, &info, &consumed, &rec);
    REQUIRE(rc == TLM_OK);
    REQUIRE(consumed == frame.size());
    return rec;
}

} // namespace

TEST_CASE("tlm crc32 matches the documented check value", "[tlm]") {
    const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(tlm_crc32(check, 9) == 0xCBF43926u);
}

TEST_CASE("tlm file header acceptance and rejection", "[tlm]") {
    std::vector<uint8_t> header(24);
    std::memcpy(header.data(), "TLM1", 4);
    tlm_write_u16(header.data() + 4, 1);  // version
    tlm_write_u16(header.data() + 6, 24); // header_size
    tlm_write_u64(header.data() + 8, 1785598530125ULL);
    tlm_write_u32(header.data() + 16, 0); // flags
    tlm_write_u32(header.data() + 20, tlm_crc32(header.data(), 20));

    tlm_file_header hdr{};
    REQUIRE(tlm_decode_file_header(header.data(), header.size(), &hdr) == TLM_OK);
    CHECK(hdr.version == 1);
    CHECK(hdr.header_size == 24);
    CHECK(hdr.created_at_ms == 1785598530125LL);

    SECTION("bad magic") {
        header[0] = 'X';
        tlm_write_u32(header.data() + 20, tlm_crc32(header.data(), 20));
        CHECK(tlm_decode_file_header(header.data(), header.size(), &hdr) == TLM_ERR_BAD_MAGIC);
    }
    SECTION("bad header crc") {
        header[20] ^= 0xFF;
        CHECK(tlm_decode_file_header(header.data(), header.size(), &hdr) == TLM_ERR_BAD_HEADER_CRC);
    }
    SECTION("bad header size") {
        tlm_write_u16(header.data() + 6, 32);
        tlm_write_u32(header.data() + 20, tlm_crc32(header.data(), 20));
        CHECK(tlm_decode_file_header(header.data(), header.size(), &hdr) == TLM_ERR_BAD_HEADER_SIZE);
    }
    SECTION("unsupported version") {
        tlm_write_u16(header.data() + 4, 2);
        tlm_write_u32(header.data() + 20, tlm_crc32(header.data(), 20));
        CHECK(tlm_decode_file_header(header.data(), header.size(), &hdr) == TLM_ERR_UNSUPPORTED_VERSION);
    }
    SECTION("reserved flags set") {
        tlm_write_u32(header.data() + 16, 1);
        tlm_write_u32(header.data() + 20, tlm_crc32(header.data(), 20));
        CHECK(tlm_decode_file_header(header.data(), header.size(), &hdr) == TLM_ERR_BAD_FLAGS);
    }
    SECTION("truncated") {
        CHECK(tlm_decode_file_header(header.data(), 10, &hdr) == TLM_ERR_TRUNCATED);
    }
}

TEST_CASE("tlm data frame decodes all standard fields", "[tlm]") {
    FrameBuilder b;
    b.str(0x01, "pump-07");
    b.str(0x02, "bearing_temp");
    std::vector<uint8_t> ts(8);
    tlm_write_u64(ts.data(), 1785598530125ULL);
    b.tlv(0x03, ts);
    std::vector<uint8_t> val(8);
    uint64_t bits = 0;
    double v = 76.4;
    std::memcpy(&bits, &v, 8);
    tlm_write_u64(val.data(), bits);
    b.tlv(0x04, val);
    b.str(0x06, "C");
    std::vector<uint8_t> q{0};
    b.tlv(0x07, q);
    std::vector<uint8_t> seq(8);
    tlm_write_u64(seq.data(), 18342);
    b.tlv(0x08, seq);
    // tags: "line=A"
    std::vector<uint8_t> tags;
    tags.push_back(4);
    tags.push_back(0);
    tags.push_back('l');
    tags.push_back('i');
    tags.push_back('n');
    tags.push_back('e');
    tags.push_back(1);
    tags.push_back(0);
    tags.push_back('A');
    b.tlv(0x09, tags);
    auto frame = b.build(1, 0, 1);

    tlm_record rec = decode_one(frame);
    CHECK(std::string(rec.device_id) == "pump-07");
    CHECK(std::string(rec.metric) == "bearing_temp");
    CHECK(rec.event_time_ms == 1785598530125);
    CHECK(rec.has_value == 1);
    CHECK(rec.value_is_null == 0);
    CHECK(rec.value == 76.4);
    CHECK(std::string(rec.unit) == "C");
    CHECK(rec.has_quality == 1);
    CHECK(rec.quality == 0);
    CHECK(rec.has_sequence == 1);
    CHECK(rec.sequence == 18342);
    REQUIRE(rec.tag_count == 1);
    CHECK(std::string(rec.tags[0].key) == "line");
    CHECK(std::string(rec.tags[0].value) == "A");
}

TEST_CASE("tlm value_null frame and missing optional fields", "[tlm]") {
    FrameBuilder b;
    b.str(0x01, "dev-01");
    b.str(0x02, "temp");
    std::vector<uint8_t> ts(8);
    tlm_write_u64(ts.data(), 1000);
    b.tlv(0x03, ts);
    b.tlv(0x05, {}); // value_null
    b.str(0x06, "C");
    auto frame = b.build();

    tlm_record rec = decode_one(frame);
    CHECK(rec.has_value == 1);
    CHECK(rec.value_is_null == 1);
    CHECK(rec.has_quality == 0);
    CHECK(rec.has_sequence == 0);
}

TEST_CASE("tlm rejects duplicate, missing and malformed TLVs", "[tlm]") {
    tlm_parser_state st{};
    tlm_frame_info info{};
    size_t consumed = 0;

    SECTION("duplicate device_id") {
        FrameBuilder b;
        b.str(0x01, "d");
        b.str(0x01, "d");
        b.str(0x02, "m");
        std::vector<uint8_t> ts(8);
        tlm_write_u64(ts.data(), 1);
        b.tlv(0x03, ts);
        b.tlv(0x05, {});
        b.str(0x06, "C");
        auto frame = b.build();
        CHECK(tlm_parse_frame(frame.data(), frame.size(), &st, &info, &consumed, nullptr) == TLM_ERR_BAD_TLV);
    }
    SECTION("missing unit") {
        FrameBuilder b;
        b.str(0x01, "d");
        b.str(0x02, "m");
        std::vector<uint8_t> ts(8);
        tlm_write_u64(ts.data(), 1);
        b.tlv(0x03, ts);
        b.tlv(0x05, {});
        auto frame = b.build();
        CHECK(tlm_parse_frame(frame.data(), frame.size(), &st, &info, &consumed, nullptr) == TLM_ERR_BAD_TLV);
    }
    SECTION("value and value_null both present") {
        FrameBuilder b;
        b.str(0x01, "d");
        b.str(0x02, "m");
        std::vector<uint8_t> ts(8);
        tlm_write_u64(ts.data(), 1);
        b.tlv(0x03, ts);
        std::vector<uint8_t> val(8);
        tlm_write_u64(val.data(), 0);
        b.tlv(0x04, val);
        b.tlv(0x05, {});
        b.str(0x06, "C");
        auto frame = b.build();
        CHECK(tlm_parse_frame(frame.data(), frame.size(), &st, &info, &consumed, nullptr) == TLM_ERR_BAD_TLV);
    }
    SECTION("truncated tlv value") {
        // Craft a frame whose TLV claims 4 value bytes but ships only 2.
        std::vector<uint8_t> bad(16);
        tlm_write_frame_header(bad.data(), 1, 0, 5, 1);
        bad.push_back(0x01);
        bad.push_back(4);
        bad.push_back(0);
        bad.push_back('a');
        bad.push_back('b');
        bad.resize(25); // room for the trailing CRC
        tlm_write_u32(bad.data() + 16 + 5, tlm_crc32(bad.data() + 16, 5));
        CHECK(tlm_parse_frame(bad.data(), bad.size(), &st, &info, &consumed, nullptr) == TLM_ERR_BAD_TLV);
    }
    SECTION("invalid device charset") {
        FrameBuilder b;
        b.str(0x01, "bad device!");
        b.str(0x02, "m");
        std::vector<uint8_t> ts(8);
        tlm_write_u64(ts.data(), 1);
        b.tlv(0x03, ts);
        b.tlv(0x05, {});
        b.str(0x06, "C");
        auto frame = b.build();
        CHECK(tlm_parse_frame(frame.data(), frame.size(), &st, &info, &consumed, nullptr) == TLM_ERR_BAD_TLV);
    }
}

TEST_CASE("tlm payload crc, truncation, size limit and sync", "[tlm]") {
    FrameBuilder b;
    b.str(0x01, "d");
    b.str(0x02, "m");
    std::vector<uint8_t> ts(8);
    tlm_write_u64(ts.data(), 1);
    b.tlv(0x03, ts);
    b.tlv(0x05, {});
    b.str(0x06, "C");
    auto frame = b.build();

    tlm_parser_state st{};
    tlm_frame_info info{};
    size_t consumed = 0;

    SECTION("payload crc mismatch") {
        auto broken = frame;
        broken[frame.size() - 1] ^= 0xFF;
        CHECK(tlm_parse_frame(broken.data(), broken.size(), &st, &info, &consumed, nullptr) == TLM_ERR_PAYLOAD_CRC);
    }
    SECTION("truncated payload") {
        CHECK(tlm_parse_frame(frame.data(), frame.size() - 2, &st, &info, &consumed, nullptr) == TLM_ERR_TRUNCATED);
    }
    SECTION("bad sync") {
        auto broken = frame;
        broken[0] = 0x00;
        CHECK(tlm_parse_frame(broken.data(), broken.size(), &st, &info, &consumed, nullptr) == TLM_ERR_BAD_SYNC);
    }
    SECTION("reserved frame flags") {
        auto broken = b.build(1, 0x02, 1);
        CHECK(tlm_parse_frame(broken.data(), broken.size(), &st, &info, &consumed, nullptr) == TLM_ERR_BAD_FLAGS);
    }
    SECTION("payload size beyond limit") {
        std::vector<uint8_t> huge(16);
        tlm_write_frame_header(huge.data(), 1, 0, TLM_MAX_PAYLOAD + 1, 1);
        CHECK(tlm_parse_frame(huge.data(), huge.size(), &st, &info, &consumed, nullptr) == TLM_ERR_PAYLOAD_TOO_LARGE);
    }
    SECTION("resync finds the next frame after garbage") {
        std::vector<uint8_t> stream;
        stream.insert(stream.end(), {'g', 'a', 'r', 'b', 'a', 'g', 'e'});
        stream.push_back(0x5A); // false sync start
        stream.insert(stream.end(), frame.begin(), frame.end());
        size_t offset = 0;
        REQUIRE(tlm_find_sync(stream.data() + 1, stream.size() - 1, 1024, &offset) == TLM_OK);
        CHECK(stream[1 + offset] == 0x5A);
        tlm_parser_state st2{};
        CHECK(stream[2 + offset] == 0xA5);
        tlm_frame_info info2{};
        tlm_record rec{};
        REQUIRE(tlm_parse_frame(stream.data() + 1 + offset, stream.size() - 1 - offset, &st2, &info2, &consumed,
                                &rec) == TLM_OK);
        CHECK(info2.sequence == 1);
    }
}

TEST_CASE("tlm sequence must strictly increase across frames", "[tlm]") {
    FrameBuilder b;
    b.str(0x01, "d");
    b.str(0x02, "m");
    std::vector<uint8_t> ts(8);
    tlm_write_u64(ts.data(), 1);
    b.tlv(0x03, ts);
    b.tlv(0x05, {});
    b.str(0x06, "C");
    auto f1 = b.build(1, 0, 5);
    auto f2 = b.build(1, 0, 3); // regression

    tlm_parser_state st{};
    tlm_frame_info info{};
    size_t consumed = 0;
    tlm_record rec{};
    REQUIRE(tlm_parse_frame(f1.data(), f1.size(), &st, &info, &consumed, &rec) == TLM_OK);
    CHECK(st.last_sequence == 5);
    REQUIRE(tlm_parse_frame(f2.data(), f2.size(), &st, &info, &consumed, &rec) == TLM_ERR_SEQUENCE_REGRESSION);
    CHECK(consumed == f2.size()); // caller can skip the offending frame
}

TEST_CASE("tlm zlib compressed payload decodes", "[tlm]") {
    // Build an uncompressed payload, deflate it, and mark the frame compressed.
    FrameBuilder b;
    b.str(0x01, "dev-01");
    b.str(0x02, "temp");
    std::vector<uint8_t> ts(8);
    tlm_write_u64(ts.data(), 1234);
    b.tlv(0x03, ts);
    b.tlv(0x05, {});
    b.str(0x06, "C");
    std::vector<uint8_t> raw = b.payload;

    uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(bound);
    uLongf compressed_len = bound;
    REQUIRE(compress2(compressed.data(), &compressed_len, raw.data(), raw.size(), Z_DEFAULT_COMPRESSION) == Z_OK);
    compressed.resize(compressed_len);

    std::vector<uint8_t> frame(16);
    tlm_write_frame_header(frame.data(), 1, TLM_FRAME_FLAG_COMPRESSED, static_cast<uint32_t>(compressed.size()), 1);
    uint32_t crc = tlm_crc32(compressed.data(), compressed.size());
    uint8_t crc_buf[4];
    tlm_write_u32(crc_buf, crc);
    frame.insert(frame.end(), compressed.begin(), compressed.end());
    frame.insert(frame.end(), crc_buf, crc_buf + 4);

    tlm_record rec = decode_one(frame);
    CHECK(std::string(rec.device_id) == "dev-01");
    CHECK(rec.value_is_null == 1);
}

TEST_CASE("tlm heartbeat and metadata frames parse without records", "[tlm]") {
    tlm_parser_state st{};
    tlm_frame_info info{};
    size_t consumed = 0;
    SECTION("heartbeat with empty payload") {
        auto frame = FrameBuilder{}.build(3, 0, 1);
        CHECK(tlm_parse_frame(frame.data(), frame.size(), &st, &info, &consumed, nullptr) == TLM_OK);
        CHECK(info.frame_type == 3);
        CHECK(consumed == frame.size());
    }
    SECTION("unknown frame type skipped") {
        auto frame = FrameBuilder{}.build(9, 0, 1);
        CHECK(tlm_parse_frame(frame.data(), frame.size(), &st, &info, &consumed, nullptr) == TLM_OK);
        CHECK(info.frame_type == 9);
    }
}

TEST_CASE("tlm unaligned buffer access", "[tlm]") {
    // Prefix one garbage byte so every field sits at odd offsets.
    FrameBuilder b;
    b.str(0x01, "d");
    b.str(0x02, "m");
    std::vector<uint8_t> ts(8);
    tlm_write_u64(ts.data(), 7);
    b.tlv(0x03, ts);
    b.tlv(0x05, {});
    b.str(0x06, "C");
    auto frame = b.build();
    std::vector<uint8_t> misaligned{'X'};
    misaligned.insert(misaligned.end(), frame.begin(), frame.end());

    size_t offset = 0;
    REQUIRE(tlm_find_sync(misaligned.data(), misaligned.size(), 8, &offset) == TLM_OK);
    CHECK(offset == 1);
    tlm_parser_state st{};
    tlm_frame_info info{};
    size_t consumed = 0;
    tlm_record rec{};
    REQUIRE(tlm_parse_frame(misaligned.data() + 1, misaligned.size() - 1, &st, &info, &consumed, &rec) == TLM_OK);
    CHECK(rec.event_time_ms == 7);
}
