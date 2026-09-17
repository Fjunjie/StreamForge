#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "streamforge/ingest/pipeline.hpp"
#include "streamforge/storage/store.hpp"
#include "support/test_env.hpp"

// Three-format consistency (acceptance §14.1 / auditor requirement ③): the committed
// binary TLM fixture and the CSV/JSONL fixtures describe the SAME four logical records;
// processing each through the pipeline must yield identical normalized samples.
using namespace streamforge;
namespace fs = std::filesystem;

namespace {

struct SampleKey {
    std::string device_id;
    std::string metric_id;
    int64_t event_time_us = 0;
    bool value_is_null = false;
    uint64_t value_bits = 0;
    int quality = 0;
    bool has_sequence = false;
    uint64_t sequence = 0;
    uint32_t flags = 0;
    std::string tags_json;
    std::string input_unit;

    bool operator<(const SampleKey& o) const {
        return std::tie(device_id, metric_id, event_time_us, value_is_null, value_bits, quality, has_sequence, sequence,
                        flags, tags_json, input_unit) <
               std::tie(o.device_id, o.metric_id, o.event_time_us, o.value_is_null, o.value_bits, o.quality,
                        o.has_sequence, o.sequence, o.flags, o.tags_json, o.input_unit);
    }
    bool operator==(const SampleKey& o) const {
        return device_id == o.device_id && metric_id == o.metric_id && event_time_us == o.event_time_us &&
               value_is_null == o.value_is_null && value_bits == o.value_bits && quality == o.quality &&
               has_sequence == o.has_sequence && sequence == o.sequence && flags == o.flags &&
               tags_json == o.tags_json && input_unit == o.input_unit;
    }
};

std::set<SampleKey> collect_samples(storage::Store& store) {
    std::set<SampleKey> out;
    auto st =
        store.db().prepare("SELECT device_id, metric_id, event_time_us, value, quality, sequence,"
                           " flags, tags_json, input_unit FROM samples ORDER BY device_id, metric_id, event_time_us");
    REQUIRE(st.ok());
    for (;;) {
        auto step = st.value().step();
        REQUIRE(step.ok());
        if (step.value() != storage::Stmt::Step::Row)
            break;
        SampleKey k;
        k.device_id = st.value().column_text(0);
        k.metric_id = st.value().column_text(1);
        k.event_time_us = st.value().column_int64(2);
        k.value_is_null = st.value().column_is_null(3);
        if (!k.value_is_null) {
            const double v = st.value().column_double(3);
            std::memcpy(&k.value_bits, &v, sizeof(k.value_bits));
        }
        k.quality = static_cast<int>(st.value().column_int64(4));
        if (!st.value().column_is_null(5)) {
            k.has_sequence = true;
            k.sequence = static_cast<uint64_t>(st.value().column_int64(5));
        }
        k.flags = static_cast<uint32_t>(st.value().column_int64(6));
        k.tags_json = st.value().column_text(7);
        if (!st.value().column_is_null(8))
            k.input_unit = st.value().column_text(8);
        out.insert(std::move(k));
    }
    return out;
}

std::string fixture_path(const std::string& name) {
    return std::string(STREAMFORGE_FIXTURES_DIR) + "/m2/" + name;
}

} // namespace

TEST_CASE("csv, jsonl and tlm produce identical normalized samples", "[consistency][pipeline][tlm]") {
    REQUIRE(fs::exists(fixture_path("consistency.tlm")));
    REQUIRE(fs::exists(fixture_path("consistency.csv")));
    REQUIRE(fs::exists(fixture_path("consistency.jsonl")));

    std::map<std::string, std::set<SampleKey>> per_format;
    for (const std::string fmt : {"csv", "jsonl", "tlm"}) {
        sf_test::TempDir dir;
        auto cfg = sf_test::make_config(dir.path);
        auto store_rc = storage::Store::open(cfg->cfg.database.path);
        REQUIRE(store_rc.ok());
        auto store = store_rc.take();
        REQUIRE(store->sync_catalog(*cfg).ok());

        // Copy the fixture into the run's input dir: the pipeline archives consumed
        // files, and the committed fixtures must survive for subsequent runs.
        const std::string src = fixture_path("consistency." + fmt);
        REQUIRE(fs::exists(src));
        const std::string input = dir.sub("input");
        std::string input_path = input;
        input_path += "/consistency.";
        input_path += fmt;
        std::ifstream in(src, std::ios::binary);
        REQUIRE(in.good());
        std::ofstream out(input_path, std::ios::binary);
        out << in.rdbuf();
        out.close();
        REQUIRE(fs::exists(input_path));

        ImportPipeline pipeline(cfg, store);
        auto result = pipeline.process_file(input_path);
        INFO(fmt << " outcome error: " << result.error.code_name() << ": " << result.error.message);
        REQUIRE(result.outcome == ImportPipeline::ProcessResult::Outcome::Completed);
        REQUIRE(result.accepted == 4);

        per_format[fmt] = collect_samples(*store);
        CHECK(per_format[fmt].size() == 4);
    }

    // All three formats yield the identical normalized sample set.
    CHECK(per_format["csv"] == per_format["jsonl"]);
    CHECK(per_format["csv"] == per_format["tlm"]);
    CHECK(per_format["jsonl"] == per_format["tlm"]);
}
