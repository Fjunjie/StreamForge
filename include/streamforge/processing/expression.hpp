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
//   Numbers:      123, 4.56, 1e10, 1.5E-3
//   Strings:      'single-quoted' (for unit comparisons in the future)
//   Identifiers:  metric references (resolved at evaluation time; may contain dots)
//   Operators:    + - * / %  (arithmetic)
//                 < > <= >= == !=  (comparison)
//                 AND OR NOT  (logical, case-insensitive keywords, short-circuit)
//   Conditional:  IF(condition, then_expr, else_expr) — lazy: only the selected
//                 branch is evaluated. IF is a reserved keyword.
//   Functions:    ABS(x), MIN(a,b,...), MAX(a,b,...), CLAMP(v,lo,hi), SQRT(x), POW(b,e)
//                 AVG(metric, duration), DELTA(metric, duration), RATE(metric, duration)
//   Duration:     integer + unit suffix (ms/s/m/h/d), e.g. 5m, 30s, 1h
//
// Type system: every subexpression evaluates to Number, Boolean or Null. Missing data
// (a metric with no current value, or a window function with insufficient history)
// evaluates to Null and propagates: an operator with a Null operand yields Null, except
// AND/OR which follow SQL three-valued logic and short-circuit when one side decides
// the result. Type mismatches (arithmetic on booleans, non-boolean IF condition) are
// errors. Parsing additionally enforces a maximum nesting depth so a pathological
// config expression cannot overflow the stack. Metric existence is checked by the
// caller (compile_rule) against the config.

enum class ExprType { Number, Boolean, Null };

struct ExprValue {
    ExprType type = ExprType::Null;
    double num = 0.0;
    bool flag = false;

    static ExprValue make_number(double v) { return {ExprType::Number, v, false}; }
    static ExprValue make_bool(bool b) { return {ExprType::Boolean, 0.0, b}; }
    static ExprValue make_null() { return {}; }
    static ExprValue from_bool(bool b) { return make_bool(b); }

    // Strict typed accessors: only the matching ExprType yields a value; Null and
    // the other type yield nullopt (callers decide whether that is an error).
    std::optional<double> as_number() const {
        if (type != ExprType::Number)
            return std::nullopt;
        return num;
    }
    std::optional<bool> as_bool() const {
        if (type != ExprType::Boolean)
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
};

using ExprPtr = std::unique_ptr<ExprNode>;

// Parses an expression string into an AST. Throws nothing; returns Error on syntax
// or structural errors (invalid number literal, unterminated string, unknown
// character, nesting too deep, duration out of range). Does NOT resolve metric
// existence (the caller checks identifiers against the config's metric set).
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
    // Returns nullopt if the metric has no current value (evaluates to Null).
    std::function<std::optional<double>(const std::string& metric)> lookup_metric;

    // Resolves a time-window aggregate for a metric over the trailing duration.
    // func is "avg", "delta" or "rate". Returns nullopt if insufficient data (Null).
    std::function<std::optional<double>(const std::string& metric, int64_t duration_us, const std::string& func)>
        lookup_window;
};

// Evaluates the AST. Returns Error on type mismatches and domain errors (division
// by zero, sqrt of negative, clamp with lo > hi); missing data yields Null instead.
Result<ExprValue> evaluate(const ExprNode& node, const EvalContext& ctx);

} // namespace expr
} // namespace streamforge
