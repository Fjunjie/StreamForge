#include "streamforge/processing/expression.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace streamforge {
namespace expr {

namespace {

[[noreturn]] Error parse_error(const std::string& msg, size_t pos) {
    throw Error::make(ErrorCode::ConfigValidation, "expression parse error: " + msg).ctx("pos", std::to_string(pos));
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Duration literals are integer + unit (ms|s|m|h|d), resolved to microseconds.
// Returns -1 when the value overflows int64 (rejected at parse time).
int64_t parse_duration(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
        ++i;
    if (i == 0 || i == text.size())
        return 0;
    int64_t num = 0;
    for (size_t k = 0; k < i; ++k) {
        int digit = text[k] - '0';
        if (num > (INT64_MAX - digit) / 10)
            return -1;
        num = num * 10 + digit;
    }
    std::string unit = to_lower(text.substr(i));
    int64_t factor = 0;
    if (unit == "ms")
        factor = 1000;
    else if (unit == "s")
        factor = 1000000;
    else if (unit == "m")
        factor = 60LL * 1000000;
    else if (unit == "h")
        factor = 3600LL * 1000000;
    else if (unit == "d")
        factor = 24LL * 3600 * 1000000;
    else
        return 0; // unknown unit (not reached: is_duration_literal gates callers)
    if (factor != 0 && num > INT64_MAX / factor)
        return -1;
    return num * factor;
}

bool is_duration_literal(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
        ++i;
    if (i == 0 || i == text.size())
        return false;
    std::string unit = to_lower(text.substr(i));
    return unit == "ms" || unit == "s" || unit == "m" || unit == "h" || unit == "d";
}

} // namespace

// -----------------------------------------------------------------------
// Tokenizer
// -----------------------------------------------------------------------

enum class Tok { End, Number, Ident, String, Op, LParen, RParen, Comma };

struct Token {
    Tok kind = Tok::End;
    std::string text;
    double num = 0.0;
    size_t pos = 0;
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}

    Token next() {
        skip_ws();
        Token t;
        t.pos = pos_;
        if (pos_ >= src_.size()) {
            t.kind = Tok::End;
            return t;
        }
        char c = src_[pos_];

        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            t.kind = Tok::Number;
            size_t start = pos_;
            while (pos_ < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.'))
                ++pos_;
            // Scientific notation: 1e10, 1.5E-3. An 'e' not followed by digits belongs
            // to a duration suffix or identifier instead (restored below).
            bool has_exponent = false;
            if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
                size_t save = pos_;
                ++pos_;
                if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
                    ++pos_;
                if (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                    while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
                        ++pos_;
                    has_exponent = true;
                } else {
                    pos_ = save;
                }
            }
            t.text = src_.substr(start, pos_ - start);
            // Duration suffix: number immediately followed by letter(s), e.g. 5m, 30s.
            if (!has_exponent && pos_ < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_]))) {
                while (pos_ < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_])))
                    ++pos_;
                t.kind = Tok::Ident; // duration literal like "5m" — parsed as ident
                t.text = src_.substr(start, pos_ - start);
                return t;
            }
            errno = 0;
            char* end = nullptr;
            t.num = std::strtod(t.text.c_str(), &end);
            if (end != t.text.c_str() + t.text.size())
                parse_error("invalid number literal '" + t.text + "'", start);
            if (errno == ERANGE)
                parse_error("number literal out of range '" + t.text + "'", start);
            return t;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            t.kind = Tok::Ident;
            size_t start = pos_;
            while (pos_ < src_.size() &&
                   (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_' || src_[pos_] == '.'))
                ++pos_;
            t.text = src_.substr(start, pos_ - start);
            return t;
        }

        if (c == '\'') {
            ++pos_; // skip opening quote
            size_t start = pos_;
            while (pos_ < src_.size() && src_[pos_] != '\'')
                ++pos_;
            if (pos_ >= src_.size())
                parse_error("unterminated string literal", start - 1);
            t.kind = Tok::String;
            t.text = src_.substr(start, pos_ - start);
            ++pos_; // skip closing quote
            return t;
        }

        // Multi-char operators
        static const std::pair<const char*, Tok> ops2[] = {
            {"<=", Tok::Op},
            {">=", Tok::Op},
            {"==", Tok::Op},
            {"!=", Tok::Op},
        };
        for (const auto& [op, kind] : ops2) {
            if (src_.compare(pos_, 2, op) == 0) {
                t.kind = kind;
                t.text = op;
                pos_ += 2;
                return t;
            }
        }

        // Single-char operators
        switch (c) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '<':
        case '>':
        case '!':
            t.kind = Tok::Op;
            t.text = std::string(1, c);
            ++pos_;
            return t;
        case '(':
            t.kind = Tok::LParen;
            ++pos_;
            return t;
        case ')':
            t.kind = Tok::RParen;
            ++pos_;
            return t;
        case ',':
            t.kind = Tok::Comma;
            ++pos_;
            return t;
        default:
            // Unknown characters must fail loudly: silently ending the token stream
            // would accept truncated expressions (e.g. "a && b" parsed as "a").
            parse_error(std::string("unexpected character '") + c + "'", pos_);
        }
    }

private:
    void skip_ws() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_])))
            ++pos_;
    }
    const std::string& src_;
    size_t pos_ = 0;
};

// -----------------------------------------------------------------------
// Recursive descent parser
// -----------------------------------------------------------------------

namespace {

class Parser {
public:
    explicit Parser(const std::string& src) : lex_(src) {
        advance();
    }

    ExprPtr parse() {
        auto result = parse_or();
        if (cur_.kind != Tok::End)
            parse_error("unexpected token '" + cur_.text + "'", cur_.pos);
        return result;
    }

private:
    // Bounds AST nesting (parens, IF/function args, unary chains) so that a
    // pathological config expression cannot overflow the stack.
    static constexpr size_t kMaxDepth = 100;

    Lexer lex_;
    Token cur_;
    size_t depth_ = 0;

    void advance() {
        cur_ = lex_.next();
    }

    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser) : p(parser) {
            if (++p.depth_ > kMaxDepth)
                parse_error("expression nesting too deep", p.cur_.pos);
        }
        ~DepthGuard() {
            --p.depth_;
        }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
    };

    bool match_op(const std::string& op) {
        if (cur_.kind == Tok::Op && cur_.text == op) {
            advance();
            return true;
        }
        // Case-insensitive keywords (AND, OR, NOT) arrive as Ident tokens.
        if (cur_.kind == Tok::Ident && to_lower(cur_.text) == op) {
            advance();
            return true;
        }
        return false;
    }

    ExprPtr parse_or() {
        auto left = parse_and();
        while (match_op("or")) {
            auto right = parse_and();
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::BinaryOp;
            node->op = "or";
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parse_and() {
        auto left = parse_not();
        while (match_op("and")) {
            auto right = parse_not();
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::BinaryOp;
            node->op = "and";
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parse_not() {
        if (match_op("not")) {
            auto child = parse_not();
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::UnaryOp;
            node->op = "not";
            node->args.push_back(std::move(child));
            return node;
        }
        return parse_comparison();
    }

    ExprPtr parse_comparison() {
        auto left = parse_additive();
        if (cur_.kind == Tok::Op) {
            std::string op = cur_.text;
            if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=") {
                advance();
                auto right = parse_additive();
                auto node = std::make_unique<ExprNode>();
                node->kind = ExprNode::Kind::BinaryOp;
                node->op = to_lower(op);
                node->args.push_back(std::move(left));
                node->args.push_back(std::move(right));
                return node;
            }
        }
        return left;
    }

    ExprPtr parse_additive() {
        auto left = parse_multiplicative();
        while (cur_.kind == Tok::Op && (cur_.text == "+" || cur_.text == "-")) {
            std::string op = cur_.text;
            advance();
            auto right = parse_multiplicative();
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::BinaryOp;
            node->op = op;
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parse_multiplicative() {
        auto left = parse_unary();
        while (cur_.kind == Tok::Op && (cur_.text == "*" || cur_.text == "/" || cur_.text == "%")) {
            std::string op = cur_.text;
            advance();
            auto right = parse_unary();
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::BinaryOp;
            node->op = op;
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parse_unary() {
        if (cur_.kind == Tok::Op && cur_.text == "-") {
            advance();
            auto child = parse_unary();
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::UnaryOp;
            node->op = "neg";
            node->args.push_back(std::move(child));
            return node;
        }
        return parse_primary();
    }

    ExprPtr parse_primary() {
        DepthGuard guard(*this);

        if (cur_.kind == Tok::Number) {
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::Number;
            node->num = cur_.num;
            advance();
            return node;
        }
        if (cur_.kind == Tok::Ident) {
            std::string text = cur_.text;
            size_t pos = cur_.pos;
            if (is_duration_literal(text)) {
                auto node = std::make_unique<ExprNode>();
                node->kind = ExprNode::Kind::DurationLit;
                node->identifier = text;
                node->duration_us = parse_duration(text);
                if (node->duration_us < 0)
                    parse_error("duration literal out of range '" + text + "'", pos);
                advance();
                return node;
            }
            if (to_lower(text) == "if") {
                advance();
                if (cur_.kind != Tok::LParen)
                    parse_error("expected '(' after IF", pos);
                advance();
                auto node = std::make_unique<ExprNode>();
                node->kind = ExprNode::Kind::Conditional;
                node->args.push_back(parse_or());
                if (cur_.kind != Tok::Comma)
                    parse_error("expected ',' in IF", pos);
                advance();
                node->args.push_back(parse_or());
                if (cur_.kind != Tok::Comma)
                    parse_error("expected ',' in IF", pos);
                advance();
                node->args.push_back(parse_or());
                if (cur_.kind != Tok::RParen)
                    parse_error("expected ')' after IF", pos);
                advance();
                return node;
            }
            advance();
            if (cur_.kind == Tok::LParen) {
                advance();
                auto node = std::make_unique<ExprNode>();
                node->kind = ExprNode::Kind::FunctionCall;
                node->identifier = to_lower(text);
                if (cur_.kind != Tok::RParen) {
                    node->args.push_back(parse_or());
                    while (cur_.kind == Tok::Comma) {
                        advance();
                        node->args.push_back(parse_or());
                    }
                }
                if (cur_.kind != Tok::RParen)
                    parse_error("expected ')' after function args", pos);
                advance();
                return node;
            }
            // Plain identifier: metric reference
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::MetricRef;
            node->identifier = text;
            return node;
        }
        if (cur_.kind == Tok::LParen) {
            advance();
            auto inner = parse_or();
            if (cur_.kind != Tok::RParen)
                parse_error("expected ')'", cur_.pos);
            advance();
            return inner;
        }
        parse_error("unexpected token '" + cur_.text + "'", cur_.pos);
    }
};

} // namespace

Result<ExprPtr> parse_expression(const std::string& source) {
    try {
        Parser p(source);
        return Result<ExprPtr>::Ok(p.parse());
    } catch (const Error& e) {
        return Result<ExprPtr>::Err(e);
    }
}

void collect_metric_refs(const ExprNode& node, std::vector<std::string>& out) {
    if (node.kind == ExprNode::Kind::MetricRef) {
        if (std::find(out.begin(), out.end(), node.identifier) == out.end())
            out.push_back(node.identifier);
    }
    for (const auto& child : node.args)
        collect_metric_refs(*child, out);
}

// -----------------------------------------------------------------------
// Evaluator
// -----------------------------------------------------------------------

namespace {

// Evaluates an argument expected to be numeric. A nullopt result means the argument
// evaluated to Null (missing data), which callers propagate.
Result<std::optional<double>> eval_number(const ExprNode& arg, const EvalContext& ctx, const std::string& fn) {
    auto v = evaluate(arg, ctx);
    if (!v.ok())
        return Result<std::optional<double>>::Err(v.error());
    if (v.value().type == ExprType::Null)
        return Result<std::optional<double>>::Ok(std::nullopt);
    auto n = v.value().as_number();
    if (!n)
        return Result<std::optional<double>>::Err(
            Error::make(ErrorCode::InvalidArgument, fn + "() requires numeric args"));
    return Result<std::optional<double>>::Ok(*n);
}

Result<ExprValue> eval_binary(const ExprNode& node, const EvalContext& ctx) {
    const std::string& op = node.op;

    // AND/OR short-circuit with SQL three-valued logic: a Boolean operand that decides
    // the result wins; Null only propagates when neither side decides.
    if (op == "and" || op == "or") {
        auto left = evaluate(*node.args[0], ctx);
        if (!left.ok())
            return left;
        std::optional<bool> lb;
        if (left.value().type == ExprType::Boolean)
            lb = left.value().as_bool();
        else if (left.value().type != ExprType::Null)
            return Result<ExprValue>::Err(
                Error::make(ErrorCode::InvalidArgument, "'" + op + "' requires boolean operands"));
        if (lb && ((op == "and" && !*lb) || (op == "or" && *lb)))
            return Result<ExprValue>::Ok(ExprValue::make_bool(*lb));

        auto right = evaluate(*node.args[1], ctx);
        if (!right.ok())
            return right;
        std::optional<bool> rb;
        if (right.value().type == ExprType::Boolean)
            rb = right.value().as_bool();
        else if (right.value().type != ExprType::Null)
            return Result<ExprValue>::Err(
                Error::make(ErrorCode::InvalidArgument, "'" + op + "' requires boolean operands"));
        if (rb && ((op == "and" && !*rb) || (op == "or" && *rb)))
            return Result<ExprValue>::Ok(ExprValue::make_bool(*rb));
        if (lb && rb)
            return Result<ExprValue>::Ok(
                ExprValue::make_bool(op == "and" ? (*lb && *rb) : (*lb || *rb)));
        return Result<ExprValue>::Ok(ExprValue::make_null());
    }

    auto left = evaluate(*node.args[0], ctx);
    if (!left.ok())
        return left;
    auto right = evaluate(*node.args[1], ctx);
    if (!right.ok())
        return right;
    const ExprValue& l = left.value();
    const ExprValue& r = right.value();

    // Missing data propagates as Null instead of failing the whole expression.
    if (l.type == ExprType::Null || r.type == ExprType::Null)
        return Result<ExprValue>::Ok(ExprValue::make_null());

    auto ln = l.as_number();
    auto rn = r.as_number();
    if (!ln || !rn)
        return Result<ExprValue>::Err(
            Error::make(ErrorCode::InvalidArgument, "operator '" + op + "' requires numeric operands"));

    double a = *ln, b = *rn;
    if (op == "+")
        return Result<ExprValue>::Ok(ExprValue::make_number(a + b));
    if (op == "-")
        return Result<ExprValue>::Ok(ExprValue::make_number(a - b));
    if (op == "*")
        return Result<ExprValue>::Ok(ExprValue::make_number(a * b));
    if (op == "/") {
        if (b == 0.0)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "division by zero"));
        return Result<ExprValue>::Ok(ExprValue::make_number(a / b));
    }
    if (op == "%") {
        if (b == 0.0)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "modulo by zero"));
        return Result<ExprValue>::Ok(ExprValue::make_number(std::fmod(a, b)));
    }
    if (op == "<")
        return Result<ExprValue>::Ok(ExprValue::make_bool(a < b));
    if (op == ">")
        return Result<ExprValue>::Ok(ExprValue::make_bool(a > b));
    if (op == "<=")
        return Result<ExprValue>::Ok(ExprValue::make_bool(a <= b));
    if (op == ">=")
        return Result<ExprValue>::Ok(ExprValue::make_bool(a >= b));
    if (op == "==")
        return Result<ExprValue>::Ok(ExprValue::make_bool(a == b));
    if (op == "!=")
        return Result<ExprValue>::Ok(ExprValue::make_bool(a != b));

    return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "unknown operator '" + op + "'"));
}

Result<ExprValue> eval_node(const ExprNode& node, const EvalContext& ctx);

Result<ExprValue> eval_function(const ExprNode& node, const EvalContext& ctx) {
    const std::string& fn = node.identifier;
    const auto& args = node.args;

    if (fn == "abs") {
        if (args.size() != 1)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "abs() requires exactly 1 arg"));
        auto n = eval_number(*args[0], ctx, "abs");
        if (!n.ok())
            return Result<ExprValue>::Err(n.error());
        if (!n.value())
            return Result<ExprValue>::Ok(ExprValue::make_null());
        return Result<ExprValue>::Ok(ExprValue::make_number(std::fabs(*n.value())));
    }
    if (fn == "min" || fn == "max") {
        if (args.size() < 2)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, fn + "() requires at least 2 args"));
        bool have = false;
        double best = 0;
        for (const auto& arg : args) {
            auto n = eval_number(*arg, ctx, fn);
            if (!n.ok())
                return Result<ExprValue>::Err(n.error());
            if (!n.value())
                return Result<ExprValue>::Ok(ExprValue::make_null());
            if (!have || (fn == "min" ? *n.value() < best : *n.value() > best)) {
                best = *n.value();
                have = true;
            }
        }
        return Result<ExprValue>::Ok(ExprValue::make_number(best));
    }
    if (fn == "clamp") {
        if (args.size() != 3)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "clamp() requires exactly 3 args"));
        auto v = eval_number(*args[0], ctx, "clamp");
        if (!v.ok())
            return Result<ExprValue>::Err(v.error());
        auto lo = eval_number(*args[1], ctx, "clamp");
        if (!lo.ok())
            return Result<ExprValue>::Err(lo.error());
        auto hi = eval_number(*args[2], ctx, "clamp");
        if (!hi.ok())
            return Result<ExprValue>::Err(hi.error());
        if (!v.value() || !lo.value() || !hi.value())
            return Result<ExprValue>::Ok(ExprValue::make_null());
        // std::clamp is UB when lo > hi; reject instead.
        if (*lo.value() > *hi.value())
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "clamp() requires lo <= hi"));
        return Result<ExprValue>::Ok(ExprValue::make_number(std::clamp(*v.value(), *lo.value(), *hi.value())));
    }
    if (fn == "sqrt") {
        if (args.size() != 1)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "sqrt() requires exactly 1 arg"));
        auto n = eval_number(*args[0], ctx, "sqrt");
        if (!n.ok())
            return Result<ExprValue>::Err(n.error());
        if (!n.value())
            return Result<ExprValue>::Ok(ExprValue::make_null());
        if (*n.value() < 0)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "sqrt() of negative number"));
        return Result<ExprValue>::Ok(ExprValue::make_number(std::sqrt(*n.value())));
    }
    if (fn == "pow") {
        if (args.size() != 2)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "pow() requires exactly 2 args"));
        auto base = eval_number(*args[0], ctx, "pow");
        if (!base.ok())
            return Result<ExprValue>::Err(base.error());
        auto exp = eval_number(*args[1], ctx, "pow");
        if (!exp.ok())
            return Result<ExprValue>::Err(exp.error());
        if (!base.value() || !exp.value())
            return Result<ExprValue>::Ok(ExprValue::make_null());
        return Result<ExprValue>::Ok(ExprValue::make_number(std::pow(*base.value(), *exp.value())));
    }

    // Time-window functions: AVG(metric, duration), DELTA(metric, duration), RATE(metric, duration)
    if (fn == "avg" || fn == "delta" || fn == "rate") {
        if (args.size() != 2)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, fn + "() requires 2 args"));
        if (args[0]->kind != ExprNode::Kind::MetricRef)
            return Result<ExprValue>::Err(
                Error::make(ErrorCode::InvalidArgument, fn + "() first arg must be a metric reference"));
        if (args[1]->kind != ExprNode::Kind::DurationLit)
            return Result<ExprValue>::Err(
                Error::make(ErrorCode::InvalidArgument, fn + "() second arg must be a duration literal"));
        const std::string& metric = args[0]->identifier;
        int64_t duration = args[1]->duration_us;
        if (!ctx.lookup_window)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "no window function provider"));
        auto val = ctx.lookup_window(metric, duration, fn);
        if (!val)
            return Result<ExprValue>::Ok(ExprValue::make_null()); // insufficient data: Null propagates
        return Result<ExprValue>::Ok(ExprValue::make_number(*val));
    }

    return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "unknown function '" + fn + "'"));
}

Result<ExprValue> eval_node(const ExprNode& node, const EvalContext& ctx) {
    switch (node.kind) {
    case ExprNode::Kind::Number:
        return Result<ExprValue>::Ok(ExprValue::make_number(node.num));

    case ExprNode::Kind::MetricRef: {
        if (!ctx.lookup_metric)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "no metric provider"));
        auto val = ctx.lookup_metric(node.identifier);
        if (!val)
            return Result<ExprValue>::Ok(ExprValue::make_null()); // no data: Null propagates
        return Result<ExprValue>::Ok(ExprValue::make_number(*val));
    }

    case ExprNode::Kind::DurationLit:
        return Result<ExprValue>::Ok(ExprValue::make_number(static_cast<double>(node.duration_us)));

    case ExprNode::Kind::BinaryOp:
        return eval_binary(node, ctx);

    case ExprNode::Kind::UnaryOp: {
        auto child = evaluate(*node.args[0], ctx);
        if (!child.ok())
            return child;
        if (child.value().type == ExprType::Null)
            return Result<ExprValue>::Ok(ExprValue::make_null());
        if (node.op == "not") {
            auto b = child.value().as_bool();
            if (!b)
                return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "NOT requires boolean operand"));
            return Result<ExprValue>::Ok(ExprValue::make_bool(!*b));
        }
        if (node.op == "neg") {
            auto n = child.value().as_number();
            if (!n)
                return Result<ExprValue>::Err(
                    Error::make(ErrorCode::InvalidArgument, "unary minus requires numeric operand"));
            return Result<ExprValue>::Ok(ExprValue::make_number(-*n));
        }
        return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "unknown unary op '" + node.op + "'"));
    }

    case ExprNode::Kind::FunctionCall:
        return eval_function(node, ctx);

    case ExprNode::Kind::Conditional: {
        auto cond = evaluate(*node.args[0], ctx);
        if (!cond.ok())
            return cond;
        if (cond.value().type == ExprType::Null)
            return Result<ExprValue>::Ok(ExprValue::make_null()); // undecided: no branch runs
        auto cb = cond.value().as_bool();
        if (!cb)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "IF condition must be boolean"));
        // Lazy: only the selected branch runs (the other may error or be expensive,
        // e.g. IF(x > 0, y / x, 0)).
        return evaluate(*node.args[*cb ? 1 : 2], ctx);
    }
    }
    return Result<ExprValue>::Err(Error::make(ErrorCode::InternalError, "unreachable"));
}

} // namespace

Result<ExprValue> evaluate(const ExprNode& node, const EvalContext& ctx) {
    return eval_node(node, ctx);
}

} // namespace expr
} // namespace streamforge
