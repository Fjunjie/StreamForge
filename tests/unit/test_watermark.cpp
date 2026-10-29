#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "streamforge/core/types.hpp"
#include "streamforge/processing/watermark.hpp"

using namespace streamforge;
using streamforge::processing::NormalizedSample;
using streamforge::processing::ReorderBuffer;

namespace {
NormalizedSample make(const std::string& device, int64_t t, uint64_t seq, double v = 1.0) {
    NormalizedSample s;
    s.device_id = device;
    s.metric_id = "temp";
    s.event_time_us = t;
    s.has_sequence = seq != 0;
    s.sequence = seq;
    s.value = v;
    return s;
}
} // namespace

TEST_CASE("watermark releases buffered samples in event-time order", "[watermark]") {
    std::vector<int64_t> emitted_times;
    ReorderBuffer buf(1000 /* lateness */, 1000, 1 << 20,
                      [&](NormalizedSample&& s) { emitted_times.push_back(s.event_time_us); });

    buf.push(make("d", 10'000'000, 1)); // wm = 10.000 - 0.001s: sample buffered
    CHECK(emitted_times.empty());
    buf.push(make("d", 10'050'000, 2)); // wm = 10.049s: releases 10.000
    std::vector<int64_t> expected{10'000'000};
    REQUIRE(emitted_times == expected);
    buf.push(make("d", 10'100'000, 3)); // wm = 10.099s: releases 10.050
    REQUIRE(emitted_times == std::vector<int64_t>{10'000'000, 10'050'000});
    buf.flush_device("d"); // releases 10.100
    CHECK(emitted_times.size() == 3);
}

TEST_CASE("late samples are emitted immediately with the late flag", "[watermark]") {
    std::vector<NormalizedSample> emitted;
    ReorderBuffer buf(1000, 1000, 1 << 20, [&](NormalizedSample&& s) { emitted.push_back(std::move(s)); });

    buf.push(make("d", 10'000'000, 1));
    buf.push(make("d", 10'100'000, 2)); // wm: 10.099s
    CHECK(emitted.size() == 1);
    buf.push(make("d", 9'000'000, 3)); // far behind the watermark: late
    REQUIRE(emitted.size() == 2);
    CHECK((emitted.back().flags & sample_flags::kLate) != 0);
    CHECK(emitted.back().event_time_us == 9'000'000);
}

TEST_CASE("capacity cap force-advances the watermark with the forced flag", "[watermark]") {
    std::vector<NormalizedSample> emitted;
    ReorderBuffer buf(60'000'000 /* 60s lateness */, 3 /* max records */, 1 << 20,
                      [&](NormalizedSample&& s) { emitted.push_back(std::move(s)); });

    for (int i = 0; i < 3; ++i) {
        buf.push(make("d", 10'000'000 + i * 1'000'000, static_cast<uint64_t>(i + 1)));
    }
    // The third push fills the buffer to the cap, which force-drains immediately.
    REQUIRE(emitted.size() == 3);
    for (const auto& s : emitted) {
        CHECK((s.flags & sample_flags::kForcedFlush) != 0);
    }
    for (size_t i = 1; i < emitted.size(); ++i) {
        CHECK(emitted[i - 1].event_time_us <= emitted[i].event_time_us);
    }
    // The buffer is empty again; further samples buffer normally.
    buf.push(make("d", 10'004'000, 4));
    CHECK(emitted.size() == 3);
}

TEST_CASE("per-device watermarks are independent", "[watermark]") {
    std::vector<std::string> emitted_devices;
    ReorderBuffer buf(1000, 1000, 1 << 20, [&](NormalizedSample&& s) { emitted_devices.push_back(s.device_id); });

    buf.push(make("a", 10'000'000, 1));
    buf.push(make("b", 5'000'000, 1));  // device b starts much older: no cross-talk
    buf.push(make("a", 10'001'500, 2)); // releases a's first sample only
    REQUIRE(emitted_devices == std::vector<std::string>{"a"});
    buf.flush_device("b"); // end-of-file style drain for b
    REQUIRE(emitted_devices == std::vector<std::string>{"a", "b"});
}

TEST_CASE("flush_device drains without the forced flag when asked", "[watermark]") {
    std::vector<NormalizedSample> emitted;
    ReorderBuffer buf(60'000'000, 1000, 1 << 20, [&](NormalizedSample&& s) { emitted.push_back(std::move(s)); });
    buf.push(make("d", 10'000'000, 1));
    buf.push(make("d", 10'001'000, 2));
    CHECK(emitted.empty());
    buf.flush_device("d", /*mark_forced=*/false);
    REQUIRE(emitted.size() == 2);
    for (const auto& s : emitted) {
        CHECK((s.flags & sample_flags::kForcedFlush) == 0);
    }
}

TEST_CASE("seeded watermark detects cross-file late arrivals", "[watermark]") {
    std::vector<NormalizedSample> emitted;
    ReorderBuffer buf(1000, 1000, 1 << 20, [&](NormalizedSample&& s) { emitted.push_back(std::move(s)); });
    buf.seed_device("d", 20'000'000);   // durable history already has 20s
    buf.push(make("d", 10'000'000, 1)); // older than the seeded watermark: late
    REQUIRE(emitted.size() == 1);
    CHECK((emitted[0].flags & sample_flags::kLate) != 0);
}
