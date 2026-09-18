#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "streamforge/processing/expression.hpp"

using namespace streamforge;
using namespace streamforge::expr;

namespace {

struct TestCtx {
    std::map<std::string, double> metrics;
    std::map<std::string, double> windows; // key: "metric|func|duration_us"

    EvalContext make() {
        EvalContext ctx;
        ctx.lookup_metric = [this](const std::string& m) -> std::optional<double> {
            auto it = metrics.find(m);
            if (it == metrics.end())
                return std::nullopt;
            return it->second;
        };
        ctx.lookup_window = [this](const std::string& m, int64_t dur, const std::string& fn) -> std::optional<double> {
            auto it = windows.find(m + "|" + fn + "|" + std::to_string(dur));
            if (it == windows.end())
                return std::nullopt;
            return it->second;
        };
        return ctx;
    }
};

// Parses and evaluates; fails the test on a parse error.
Result<ExprValue> eval_src(const std::string& src, const EvalContext& ctx) {
    auto ast = parse_expression(src);
    REQUIRE(ast.ok());
    return evaluate(*ast.value(), ctx);
}

} // namespace

TEST_CASE("expression rejects malformed input with clear errors", "[expression]") {
    auto msg_of = [](const std::string& src) {
        auto ast = parse_expression(src);
        REQUIRE_FALSE(ast.ok());
        return ast.error().message;
    };

    CHECK(msg_of("1.2.3").find("invalid number") != std::string::npos);
    CHECK(msg_of("1..2").find("invalid number") != std::string::npos);
    CHECK(msg_of(".").find("invalid number") != std::string::npos);
    CHECK(msg_of("1e999").find("out of range") != std::string::npos);
    CHECK(msg_of("99999999999999999999d").find("out of range") != std::string::npos); // digits overflow
    CHECK(msg_of("200000000d").find("out of range") != std::string::npos);           // unit multiply overflows
    CHECK(msg_of("'abc").find("unterminated string") != std::string::npos);
    // Unknown characters must fail loudly instead of silently truncating the
    // expression ("a && b" must not parse as "a").
    CHECK(msg_of("temp > 80 && load > 1").find("unexpected character") != std::string::npos);
    CHECK(msg_of("x = 5").find("unexpected character") != std::string::npos);
    CHECK(msg_of("a | b").find("unexpected character") != std::string::npos);
    CHECK(msg_of("temp >").find("unexpected token") != std::string::npos);
    CHECK(msg_of("").find("unexpected token") != std::string::npos);

    // Deep nesting is rejected instead of overflowing the stack.
    std::string deep(300, '(');
    deep += "1";
    deep.append(300, ')');
    CHECK(msg_of(deep).find("too deep") != std::string::npos);
}

TEST_CASE("expression number formats and durations", "[expression]") {
    TestCtx t;
    auto ctx = t.make();

    auto v = eval_src("1e10", ctx);
    REQUIRE(v.ok());
    CHECK(v.value().num == 1e10);

    v = eval_src("1.5E-3", ctx);
    REQUIRE(v.ok());
    CHECK(v.value().num == 0.0015);

    v = eval_src("5m", ctx); // duration literal evaluates to microseconds
    REQUIRE(v.ok());
    CHECK(v.value().num == 300.0 * 1000000);

    v = eval_src("0s", ctx);
    REQUIRE(v.ok());
    CHECK(v.value().num == 0.0);
}

TEST_CASE("expression precedence and logic", "[expression]") {
    TestCtx t;
    auto ctx = t.make();

    CHECK(eval_src("2 + 3 * 4", ctx).value().num == 14);
    CHECK(eval_src("(2 + 3) * 4", ctx).value().num == 20);
    CHECK(eval_src("10 % 3", ctx).value().num == 1);
    CHECK(eval_src("-5 + 3", ctx).value().num == -2);
    CHECK(eval_src("1 < 2 AND 3 > 2", ctx).value().flag);
    CHECK(eval_src("not 1 > 2", ctx).value().flag);
    CHECK(eval_src("1 < 2 or 3 < 2", ctx).value().flag);
    CHECK(eval_src("1 == 1 and 2 != 3", ctx).value().flag);
}

TEST_CASE("expression IF is lazy and case-insensitive", "[expression]") {
    TestCtx t;
    auto ctx = t.make();

    CHECK(eval_src("IF(1 < 2, 10, 20)", ctx).value().num == 10);
    CHECK(eval_src("if(2 > 1, 7, 9)", ctx).value().num == 7);

    // The untaken branch must not be evaluated: division by zero stays hidden.
    auto ok = eval_src("IF(1 < 2, 5, 1 / 0)", ctx);
    REQUIRE(ok.ok());
    CHECK(ok.value().num == 5);

    auto err = eval_src("IF(1 > 2, 5, 1 / 0)", ctx);
    REQUIRE_FALSE(err.ok());
    CHECK(err.error().message.find("division by zero") != std::string::npos);
}

TEST_CASE("expression Null propagation", "[expression]") {
    TestCtx t; // no metrics: every lookup misses
    auto ctx = t.make();

    auto v = eval_src("temp > 80", ctx);
    REQUIRE(v.ok());
    CHECK(v.value().type == ExprType::Null);

    v = eval_src("temp + 1", ctx);
    REQUIRE(v.ok());
    CHECK(v.value().type == ExprType::Null);

    // SQL three-valued logic with short-circuit: a deciding operand wins.
    CHECK(eval_src("temp > 1 or 2 > 1", ctx).value().flag);
    CHECK(eval_src("2 < 1 and temp > 1", ctx).value().flag == false);
    CHECK(eval_src("temp > 1 or 2 < 1", ctx).value().type == ExprType::Null);
    CHECK(eval_src("2 > 1 and temp > 1", ctx).value().type == ExprType::Null);

    v = eval_src("IF(temp > 1, 1, 2)", ctx);
    REQUIRE(v.ok());
    CHECK(v.value().type == ExprType::Null);

    v = eval_src("ABS(temp)", ctx);
    REQUIRE(v.ok());
    CHECK(v.value().type == ExprType::Null);
}

TEST_CASE("expression type and domain errors", "[expression]") {
    TestCtx t;
    auto ctx = t.make();

    auto check_err = [&](const std::string& src, const std::string& needle) {
        auto v = eval_src(src, ctx);
        REQUIRE_FALSE(v.ok());
        INFO(v.error().message);
        CHECK(v.error().message.find(needle) != std::string::npos);
    };

    check_err("1 and 2", "boolean");
    check_err("not 5", "boolean");
    check_err("IF(5, 1, 2)", "boolean");
    check_err("1 / 0", "division by zero");
    check_err("5 % 0", "modulo by zero");
    check_err("SQRT(-1)", "negative");
    check_err("CLAMP(5, 10, 0)", "lo <= hi"); // std::clamp would be UB
    check_err("ABS()", "exactly 1 arg");      // empty args must not dereference args[0]
    check_err("SQRT()", "exactly 1 arg");
    check_err("MIN(1)", "at least 2");
    check_err("POW(1, 2, 3)", "exactly 2");
    check_err("FOO(1)", "unknown function");
    check_err("1 + (1 < 2)", "numeric"); // boolean operand in arithmetic
}

TEST_CASE("expression math functions", "[expression]") {
    TestCtx t;
    auto ctx = t.make();

    CHECK(eval_src("ABS(-3)", ctx).value().num == 3);
    CHECK(eval_src("MIN(3, 1, 2)", ctx).value().num == 1);
    CHECK(eval_src("MAX(3, 1, 2)", ctx).value().num == 3);
    CHECK(eval_src("CLAMP(15, 0, 10)", ctx).value().num == 10);
    CHECK(eval_src("CLAMP(-5, 0, 10)", ctx).value().num == 0);
    CHECK(eval_src("CLAMP(5, 0, 10)", ctx).value().num == 5);
    CHECK(eval_src("SQRT(9)", ctx).value().num == 3);
    CHECK(eval_src("POW(2, 10)", ctx).value().num == 1024);
}

TEST_CASE("expression window functions", "[expression]") {
    TestCtx t;
    auto ctx = t.make();

    t.windows["temp|avg|300000000"] = 42.5;
    auto v = eval_src("AVG(temp, 5m)", ctx);
    REQUIRE(v.ok());
    CHECK(v.value().num == 42.5);

    // Insufficient history: Null, not an error.
    v = eval_src("AVG(pressure, 5m)", ctx);
    REQUIRE(v.ok());
    CHECK(v.value().type == ExprType::Null);

    auto err = eval_src("AVG(5, 5m)", ctx);
    REQUIRE_FALSE(err.ok());
    err = eval_src("AVG(temp, 5)", ctx);
    REQUIRE_FALSE(err.ok());

    // No window provider at all: explicit error.
    EvalContext bare;
    auto ast = parse_expression("AVG(temp, 5m)");
    REQUIRE(ast.ok());
    auto no_provider = evaluate(*ast.value(), bare);
    REQUIRE_FALSE(no_provider.ok());
}

TEST_CASE("collect_metric_refs dedupes in order", "[expression]") {
    auto ast = parse_expression("IF(temp > pressure, flow / temp, 0)");
    REQUIRE(ast.ok());
    std::vector<std::string> refs;
    collect_metric_refs(*ast.value(), refs);
    REQUIRE(refs.size() == 3);
    CHECK(refs[0] == "temp");
    CHECK(refs[1] == "pressure");
    CHECK(refs[2] == "flow");
}
