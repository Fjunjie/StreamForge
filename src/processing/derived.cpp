#include "streamforge/processing/derived.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <spdlog/spdlog.h>

#include "streamforge/core/log.hpp"

namespace streamforge {
namespace processing {

Result<DerivedCalculator> DerivedCalculator::create(const ConfigSnapshot& cs) {
    DerivedCalculator calc;
    std::map<std::string, size_t> index; // derived id -> node index (compile order)

    for (const auto& dm : cs.cfg.derived_metrics) {
        Node node;
        node.id = dm.id;
        node.unit = dm.unit;
        node.missing_policy = dm.missing_policy;
        node.max_input_age_us = dm.max_input_age_us;

        auto ast = expr::parse_expression(dm.expression);
        if (!ast.ok()) {
            Error e = ast.error();
            e.ctx("derived_metric", dm.id);
            return Result<DerivedCalculator>::Err(std::move(e));
        }
        node.ast = ast.take();
        expr::collect_metric_refs(*node.ast, node.inputs);

        // Every reference must resolve to a base metric or another derived metric.
        for (const auto& ref : node.inputs) {
            if (cs.metric(ref) != nullptr)
                continue;
            bool is_derived = false;
            for (const auto& other : cs.cfg.derived_metrics) {
                if (other.id == ref) {
                    is_derived = true;
                    break;
                }
            }
            if (!is_derived) {
                Error e = Error::make(ErrorCode::ConfigValidation,
                                      "derived metric references unknown metric '" + ref + "'");
                e.ctx("derived_metric", dm.id);
                e.ctx("metric", ref);
                return Result<DerivedCalculator>::Err(std::move(e));
            }
        }
        if (std::find(node.inputs.begin(), node.inputs.end(), node.id) != node.inputs.end()) {
            Error e = Error::make(ErrorCode::ConfigValidation,
                                  "derived metric '" + dm.id + "' depends on itself");
            e.ctx("derived_metric", dm.id);
            return Result<DerivedCalculator>::Err(std::move(e));
        }
        index[node.id] = calc.nodes_.size();
        calc.nodes_.push_back(std::move(node));
    }

    // Topological order (Kahn) over derived-on-derived edges; a leftover node means a
    // dependency cycle, rejected here so the config never reaches runtime (FR-DER-002).
    const size_t n = calc.nodes_.size();
    std::vector<std::vector<size_t>> adj(n);
    std::vector<size_t> indegree(n, 0);
    for (size_t i = 0; i < n; ++i) {
        for (const auto& ref : calc.nodes_[i].inputs) {
            auto it = index.find(ref);
            if (it != index.end() && it->second != i) {
                adj[it->second].push_back(i);
                ++indegree[i];
            }
        }
    }
    std::vector<size_t> order;
    order.reserve(n);
    std::vector<size_t> ready;
    for (size_t i = 0; i < n; ++i)
        if (indegree[i] == 0)
            ready.push_back(i);
    while (!ready.empty()) {
        size_t u = ready.back();
        ready.pop_back();
        order.push_back(u);
        for (size_t v : adj[u])
            if (--indegree[v] == 0)
                ready.push_back(v);
    }
    if (order.size() != n) {
        return Result<DerivedCalculator>::Err(Error::make(
            ErrorCode::ConfigValidation, "derived metric dependency graph contains a cycle"));
    }
    std::vector<Node> ordered;
    ordered.reserve(n);
    for (size_t idx : order)
        ordered.push_back(std::move(calc.nodes_[idx]));
    calc.nodes_ = std::move(ordered);

    // Transitive input closure per node; topological order lets each node extend its
    // closure with the (already computed) closures of its derived inputs.
    std::map<std::string, size_t> final_index;
    for (size_t i = 0; i < calc.nodes_.size(); ++i)
        final_index[calc.nodes_[i].id] = i;
    std::vector<std::set<std::string>> closures(calc.nodes_.size());
    for (size_t i = 0; i < calc.nodes_.size(); ++i) {
        auto& closure = closures[i];
        for (const auto& ref : calc.nodes_[i].inputs) {
            closure.insert(ref);
            auto it = final_index.find(ref);
            if (it != final_index.end() && it->second < i) {
                for (const auto& m : closures[it->second])
                    closure.insert(m);
            }
        }
        calc.nodes_[i].transitive_inputs.assign(closure.begin(), closure.end());
    }

    return Result<DerivedCalculator>::Ok(std::move(calc));
}

std::vector<DerivedCalculator::Output> DerivedCalculator::observe(const NormalizedSample& s) {
    std::vector<Output> outs;
    auto& dev_state = state_[s.device_id];

    auto cur = dev_state.find(s.metric_id);
    if (cur != dev_state.end() && s.event_time_us < cur->second.latest.time_us)
        return outs; // late sample: the state keeps moving forward
    MetricState& st = dev_state[s.metric_id];
    st.latest = InputValue{s.value, s.event_time_us, s.value_is_null};
    if (!s.value_is_null) {
        st.last_good = st.latest;
        st.has_good = true;
    }

    for (const auto& node : nodes_) {
        const auto& affected = node.transitive_inputs;
        if (std::find(affected.begin(), affected.end(), s.metric_id) == affected.end())
            continue; // not influenced by the observed metric
        auto out = evaluate_node(node, s.device_id, s.event_time_us);
        if (!out)
            continue;
        outs.push_back(*out);
        MetricState& derived_state = dev_state[node.id];
        derived_state.latest = InputValue{out->value, out->event_time_us, out->value_is_null};
        if (!out->value_is_null) {
            derived_state.last_good = derived_state.latest;
            derived_state.has_good = true;
        }
    }
    return outs;
}

std::optional<DerivedCalculator::Output> DerivedCalculator::evaluate_node(const Node& node,
                                                                          const std::string& device_id,
                                                                          int64_t eval_time_us) {
    static const std::map<std::string, MetricState> kEmptyState;
    auto it = state_.find(device_id);
    const std::map<std::string, MetricState>& dev_state = it != state_.end() ? it->second : kEmptyState;

    // Resolves an input value. With ignore_age the last non-null value counts no matter
    // how old it is (the 'recent' fallback); otherwise the latest reading counts only
    // when it is non-null and within max_input_age_us.
    auto value_of = [&](const std::string& metric, bool ignore_age) -> std::optional<double> {
        auto v = dev_state.find(metric);
        if (v == dev_state.end())
            return std::nullopt;
        const MetricState& st = v->second;
        if (ignore_age)
            return st.has_good ? std::optional<double>(st.last_good.value) : std::nullopt;
        if (st.latest.is_null)
            return std::nullopt;
        if (node.max_input_age_us > 0 && eval_time_us - st.latest.time_us > node.max_input_age_us)
            return std::nullopt;
        return st.latest.value;
    };

    const bool recent = node.missing_policy == "recent";
    if (!recent) {
        // 'null' and 'skip' require every direct input to be usable right now.
        for (const auto& in : node.inputs) {
            if (value_of(in, /*ignore_age=*/false))
                continue;
            if (node.missing_policy == "null") {
                Output o;
                o.metric_id = node.id;
                o.unit = node.unit;
                o.event_time_us = eval_time_us;
                o.value_is_null = true;
                return o;
            }
            return std::nullopt; // 'skip'
        }
    }

    expr::EvalContext ctx;
    ctx.device_id = device_id;
    ctx.eval_time_us = eval_time_us;
    ctx.lookup_metric = [&value_of, &recent](const std::string& m) -> std::optional<double> {
        return value_of(m, recent);
    };
    auto v = expr::evaluate(*node.ast, ctx);
    if (!v.ok()) {
        ++compute_errors_;
        SPDLOG_LOGGER_DEBUG(logger("processing"), "derived metric '{}' computation error: {}", node.id,
                            v.error().message);
        return std::nullopt;
    }
    Output o;
    o.metric_id = node.id;
    o.unit = node.unit;
    o.event_time_us = eval_time_us;
    if (v.value().type == expr::ExprType::Null) {
        o.value_is_null = true; // e.g. 'recent' with an input never seen
        return o;
    }
    if (v.value().type != expr::ExprType::Number || !std::isfinite(v.value().num)) {
        ++compute_errors_; // non-numeric or non-finite result: explicit compute error
        return std::nullopt;
    }
    o.value = v.value().num;
    return o;
}

} // namespace processing
} // namespace streamforge
