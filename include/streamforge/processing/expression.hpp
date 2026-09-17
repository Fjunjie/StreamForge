#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "streamforge/core/error.hpp"

namespace streamforge {
namespace expr {

// Restricted expression language for derived metrics and rule conditions (FR-DER-001).
//
// Supported syntax:
//   Numbers:      123, 4.56, 1e10
//   Strings:      'single-quoted' (for unit comparisons in the future)
//   Identifiers:  metric references (resolved at evaluation time)
//   Operators:    + - * / %  (arithmetic)
//                 < > <= >= == !=  (comparison)
//                 AND OR NOT  (logical, case-insensitive keywords)
//   Conditional:  IF(condition, then_expr, else_expr)
//   Functions:    ABS(x), MIN(a,b), MAX(a,b), CLAMP(v,lo,hi), SQRT(x), POW(b,e)
//                 AVG(metric, duration), DELTA(metric, duration), RATE(metric, duration)
//   Duration:     integer + unit suffix (ms/s/m/h/d), e.g. 5m, 30s, 1h
//
// Type system: every subexpression evaluates to Number, Boolean or Null.
// Arithmetic requires Number operands (Null propagates). Comparisons require
// same-type operands. Logical operators require Boolean. The type checker runs
// at parse time (structural checks only — metric existence is checked separately
// against the config). Unit checking is done by the caller using the dimension info.

enum class ExprType { Number, Boolean, Null };

struct ExprValue {
    ExprType type = ExprType::Null;
    double num = 0.0;
    bool flag = false;

    static ExprValue make_number(double v) { return {ExprType::Number, v, false}; }
    static ExprValue make_bool(bool b) { return {ExprType::Boolean, 0.0, b}; }
    static ExprValue make_null() { return {}; }
    static ExprValue from_bool(bool b) { return make_bool(b); }

    // Returns numeric value; Null propagates as nullopt.
    std::optional<double> as_number() const {
        if (type == ExprType::Null)
            return std::nullopt;
        return num;
    }
    std::optional<bool> as_bool() const {
        if (type == ExprType::Null)
            return std::nullopt;
        return flag;
    }
    bool operator==(const ExprValue& o) const { return type == o.type && num == o.num && flag == o.flag; }
};

// AST node.
struct ExprNode {
    enum class Kind {
        Number,
        MetricRef,   // identifier: resolves to a metric sample value
        DurationLit, // duration literal: value in microseconds
        BinaryOp,
        UnaryOp,
        FunctionCall,
        Conditional,
    };
    Kind kind;
    double num = 0.0;        // Number
    std::string identifier;  // MetricRef or DurationLit (raw text) or FunctionCall name
    std::string op;          // BinaryOp / UnaryOp operator
    int64_t duration_us = 0; // DurationLit resolved value
    std::vector<std::unique_ptr<ExprNode>> args;

    // Compile-time dimension for metric refs (set during type check against config).
    // -1 = not checked / N/A. Non-negative = core_units::Dimension value.
    int metric_dimension = -1;
};

using ExprPtr = std::unique_ptr<ExprNode>;

// Parses an expression string into an AST. Throws nothing; returns Error on syntax
// or structural type errors. Does NOT resolve metric existence (the caller checks
// identifiers against the config's metric set).
Result<ExprPtr> parse_expression(const std::string& source);

// Collects all metric identifiers referenced in the AST (deduped, in order of
// first appearance). Used to verify referenced metrics exist and to build the
// evaluation context.
void collect_metric_refs(const ExprNode& node, std::vector<std::string>& out);

// Evaluation context: provides metric values and time-window functions.
struct EvalContext {
    std::string device_id;
    int64_t eval_time_us = 0;

    // Resolves a metric's current value (canonical unit) for the context device.
    // Returns nullopt if the metric has no current value.
    std::function<std::optional<double>(const std::string& metric)> lookup_metric;

    // Resolves a time-window aggregate for a metric over the trailing duration.
    // func is "avg", "delta" or "rate". Returns nullopt if insufficient data.
    std::function<std::optional<double>(const std::string& metric, int64_t duration_us, const std::string& func)>
        lookup_window;
};

// Evaluates the AST. Returns Error on runtime failures (unknown metric reference
// with no Null default, division by zero, sqrt of negative, etc.).
Result<ExprValue> evaluate(const ExprNode& node, const EvalContext& ctx);

} // namespace expr
} // namespace streamforge
