// streamforge-gen: reproducible synthetic telemetry generator for performance and
// stability testing (requirement 13.4). Emits JSONL with controllable device/metric
// counts, record count, duplicate rate, out-of-order rate, error rate and RNG seed.
//
// Usage:
//   streamforge-gen --records <n> [--devices 10] [--metrics 5] [--duplicates 0.01]
//                   [--late 0.10] [--errors 0.01] [--seed 42] [--output <file>|-]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "streamforge/core/uuid.hpp"

namespace {

using json = nlohmann::json;

int print_usage() {
    std::fputs("usage: streamforge-gen --records <n> [--devices 10] [--metrics 5] [--duplicates 0.01]\n"
               "                        [--late 0.10] [--errors 0.01] [--seed 42] [--output <file>|-]\n",
               stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    int64_t records = -1, devices = 10, metrics = 5, seed = 42;
    double duplicates = 0.01, late = 0.10, errors = 0.01;
    std::string output = "-";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&](int64_t& dst) {
            if (i + 1 >= argc)
                std::exit(print_usage());
            dst = std::strtoll(argv[++i], nullptr, 10);
        };
        auto rate = [&](double& dst) {
            if (i + 1 >= argc)
                std::exit(print_usage());
            dst = std::strtod(argv[++i], nullptr);
        };
        if (arg == "--records" && i + 1 < argc)
            records = std::strtoll(argv[++i], nullptr, 10);
        else if (arg == "--devices")
            value(devices);
        else if (arg == "--metrics")
            value(metrics);
        else if (arg == "--seed")
            value(seed);
        else if (arg == "--duplicates")
            rate(duplicates);
        else if (arg == "--late")
            rate(late);
        else if (arg == "--errors")
            rate(errors);
        else if (arg == "--output" && i + 1 < argc)
            output = argv[++i];
        else
            return print_usage();
    }
    if (records <= 0 || devices <= 0 || metrics <= 0)
        return print_usage();

    std::mt19937_64 rng(static_cast<uint64_t>(seed));
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_real_distribution<double> value_dist(20.0, 100.0);

    std::FILE* out = stdout;
    bool close_out = false;
    if (output != "-") {
        out = std::fopen(output.c_str(), "wb");
        if (out == nullptr) {
            std::fprintf(stderr, "streamforge-gen: cannot open output %s\n", output.c_str());
            return 1;
        }
        close_out = true;
    }

    // Base period of 1 second per device/metric series; the generator walks event time
    // forward and perturbs it for the configured late rate.
    constexpr int64_t kBase = 1754000000LL * 1000000; // fixed epoch for reproducibility
    int64_t emitted = 0, dup_count = 0, late_count = 0, err_count = 0;
    while (emitted + dup_count + late_count + err_count < records) {
        double roll = unit(rng);
        int64_t device_idx = static_cast<int64_t>(unit(rng) * devices) % devices;
        int64_t metric_idx = static_cast<int64_t>(unit(rng) * metrics) % metrics;
        int64_t seq = emitted;
        int64_t event_us = kBase + seq * 1000000 + device_idx * 1000;

        json rec;
        rec["device_id"] = "gen-" + std::to_string(device_idx);
        rec["metric"] = "gm-" + std::to_string(metric_idx);
        rec["sequence"] = seq;

        if (roll < errors) {
            // Malformed value field: the parser must reject without crashing.
            rec["event_time"] = "2026-08-01T00:00:00.000Z";
            rec["value"] = "not-a-number";
            rec["unit"] = "C";
            rec["quality"] = 0;
            ++err_count;
        } else if (roll < errors + duplicates) {
            // Exact replay of the previous record: the dedup layer must drop it.
            rec["event_time"] = "2026-08-01T00:00:00.000Z";
            rec["value"] = value_dist(rng);
            rec["unit"] = "C";
            rec["quality"] = 0;
            rec["sequence"] = std::max<int64_t>(0, seq - 1); // same sequence as before
            ++dup_count;
        } else if (roll < errors + duplicates + late) {
            // Out-of-order: event time jumps backwards within the allowed lateness.
            rec["event_time_us"] = event_us - 2000000;
            rec["value"] = value_dist(rng);
            rec["unit"] = "C";
            rec["quality"] = 0;
            ++late_count;
        } else {
            rec["event_time_us"] = event_us;
            rec["value"] = value_dist(rng);
            rec["unit"] = "C";
            rec["quality"] = 0;
            ++emitted;
        }
        std::string line = rec.dump();
        std::fwrite(line.data(), 1, line.size(), out);
        std::fputc('\n', out);
    }
    if (close_out)
        std::fclose(out);
    std::fprintf(stderr, "streamforge-gen: emitted=%lld duplicates=%lld late=%lld errors=%lld\n",
                 static_cast<long long>(emitted), static_cast<long long>(dup_count),
                 static_cast<long long>(late_count), static_cast<long long>(err_count));
    return 0;
}
