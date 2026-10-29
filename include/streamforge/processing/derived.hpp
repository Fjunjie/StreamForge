#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "streamforge/config/config.hpp"
#include "streamforge/core/error.hpp"
#include "streamforge/processing/expression.hpp"
#include "streamforge/processing/types.hpp"

namespace streamforge {
namespace processing {

// Derived-metric computation over the restricted expression language (FR-DER-001/002).
//
// Semantics:
//  - Event driven: observe() is called for every freshly committed real sample; every
//    derived metric whose (transitive) input closure contains the observed metric is
//    recomputed at the sample's event time, in topological order so derived-on-derived
//    chains see the just-computed values. Derived outputs update the per-device state
//    and are returned to the caller for insertion; they never re-trigger observe().
//  - Input usability: the latest known value of an input counts as usable when it is
//    not null and (max_input_age_us <= 0 or event_time - value_time <= max_input_age_us).
//  - missing_policy decides what happens when not all inputs are usable:
//      null  -> emit a NULL-valued sample;
//      skip  -> emit nothing;
//      recent-> recompute using the latest known values regardless of age; inputs that
//               were never seen yield Null and thus a NULL-valued sample.
//  - Computation errors (division by zero, sqrt of negative, non-finite results) are
//    counted in compute_errors() and produce no sample; they never crash the service.
//  - Late samples (event time older than the metric's stored value) are ignored so the
//    state keeps moving forward.
class DerivedCalculator {
public:
    struct Output {
        std::string metric_id;
        std::string unit;
        int64_t event_time_us = 0;
        bool value_is_null = false;
        double value = 0.0;
    };

    // Compiles and validates the config's derived metrics: parses every expression,
    // checks that all referenced metrics exist (base or derived) and that the derived
    // dependency graph is acyclic. Returns the calculator in topological evaluation
    // order, or a config validation error.
    static Result<DerivedCalculator> create(const ConfigSnapshot& cs);

    // Processes one committed real sample; returns the derived samples it triggers.
    [[nodiscard]] std::vector<Output> observe(const NormalizedSample& s);

    [[nodiscard]] uint64_t compute_errors() const { return compute_errors_; }

private:
    DerivedCalculator() = default;

    struct InputValue {
        double value = 0.0;
        int64_t time_us = 0;
        bool is_null = true;
    };

    // Per-metric state: the latest reading (null readings advance its time) plus the
    // most recent non-null value, which is what the 'recent' policy falls back to.
    struct MetricState {
        InputValue latest;
        InputValue last_good; // meaningful only when has_good
        bool has_good = false;
    };

    struct Node {
        std::string id;
        std::string unit;
        expr::ExprPtr ast;
        std::string missing_policy; // null|recent|skip
        int64_t max_input_age_us = 0;
        std::vector<std::string> inputs;        // direct metric references
        std::vector<std::string> transitive_inputs; // includes own input closure
    };

    // Evaluates one node for `device` at `eval_time`; returns nullopt when the policy
    // says the node must not produce output this time (skip / error).
    std::optional<Output> evaluate_node(const Node& node, const std::string& device_id, int64_t eval_time_us);

    std::vector<Node> nodes_; // topological order
    // device -> metric -> state
    std::map<std::string, std::map<std::string, MetricState>> state_;
    uint64_t compute_errors_ = 0;
};

} // namespace processing
} // namespace streamforge
