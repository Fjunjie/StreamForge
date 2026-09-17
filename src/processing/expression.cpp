#include "streamforge/processing/expression.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "streamforge/core/units.hpp"

namespace streamforge {
namespace expr {

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
            // Duration suffix: number immediately followed by letter(s)
            if (pos_ < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_]))) {
                while (pos_ < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_])))
                    ++pos_;
                t.kind = Tok::Ident; // duration literal like "5m" — parsed as ident
                t.text = src_.substr(start, pos_ - start);
                return t;
            }
            t.text = src_.substr(start, pos_ - start);
            t.num = std::strtod(t.text.c_str(), nullptr);
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
            t.kind = Tok::String;
            ++pos_; // skip opening quote
            size_t start = pos_;
            while (pos_ < src_.size() && src_[pos_] != '\'')
                ++pos_;
            t.text = src_.substr(start, pos_ - start);
            if (pos_ < src_.size())
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
        default: {
            t.kind = Tok::End; // unknown character: signal end (parse error will be raised)
            return t;
        }
        }
    }

    Token peek() {
        size_t save = pos_;
        Token t = next();
        pos_ = save;
        return t;
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

[[noreturn]] Error parse_error(const std::string& msg, size_t pos) {
    throw Error::make(ErrorCode::ConfigValidation, "expression parse error: " + msg).ctx("pos", std::to_string(pos));
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

int64_t parse_duration(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
        ++i;
    if (i == 0 || i == text.size())
        return 0;
    int64_t num = 0;
    for (size_t k = 0; k < i; ++k)
        num = num * 10 + (text[k] - '0');
    std::string unit = to_lower(text.substr(i));
    if (unit == "ms")
        return num * 1000;
    if (unit == "s")
        return num * 1000000;
    if (unit == "m")
        return num * 60 * 1000000;
    if (unit == "h")
        return num * 3600 * 1000000;
    if (unit == "d")
        return num * 24 * 3600 * 1000000;
    return 0; // unknown unit
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

class Parser {
public:
    explicit Parser(const std::string& src) : lex_(src) { advance(); }

    ExprPtr parse() {
        auto result = parse_or();
        if (cur_.kind != Tok::End)
            parse_error("unexpected token '" + cur_.text + "'", cur_.pos);
        return result;
    }

private:
    Lexer lex_;
    Token cur_;

    void advance() { cur_ = lex_.next(); }

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

    bool match_keyword(const std::string& kw) {
        if (cur_.kind == Tok::Ident && to_lower(cur_.text) == kw) {
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
        if (cur_.kind == Tok::Ident && to_lower(cur_.text) == "if") {
            advance();
            if (cur_.kind != Tok::LParen)
                parse_error("expected '(' after IF", cur_.pos);
            advance();
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::Conditional;
            node->args.push_back(parse_or());
            if (cur_.kind != Tok::Comma)
                parse_error("expected ',' in IF", cur_.pos);
            advance();
            node->args.push_back(parse_or());
            if (cur_.kind != Tok::Comma)
                parse_error("expected ',' in IF", cur_.pos);
            advance();
            node->args.push_back(parse_or());
            if (cur_.kind != Tok::RParen)
                parse_error("expected ')' after IF", cur_.pos);
            advance();
            return node;
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

Result<ExprValue> eval_binary(const ExprNode& node, const EvalContext& ctx) {
    auto left = evaluate(*node.args[0], ctx);
    if (!left.ok())
        return left;
    auto right = evaluate(*node.args[1], ctx);
    if (!right.ok())
        return right;
    const ExprValue& l = left.value();
    const ExprValue& r = right.value();

    const std::string& op = node.op;
    if (op == "and") {
        auto lb = l.as_bool();
        auto rb = r.as_bool();
        if (!lb || !rb)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "'and' requires boolean operands"));
        return Result<ExprValue>::Ok(ExprValue::make_bool(*lb && *rb));
    }
    if (op == "or") {
        auto lb = l.as_bool();
        auto rb = r.as_bool();
        if (!lb || !rb)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "'or' requires boolean operands"));
        return Result<ExprValue>::Ok(ExprValue::make_bool(*lb || *rb));
    }

    // All remaining operators require numeric operands (Null propagates as error).
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
        auto v = evaluate(*args[0], ctx);
        if (!v.ok())
            return v;
        auto n = v.value().as_number();
        if (!n)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "abs() requires numeric arg"));
        return Result<ExprValue>::Ok(ExprValue::make_number(std::fabs(*n)));
    }
    if (fn == "min" || fn == "max") {
        if (args.size() < 2)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, fn + "() requires at least 2 args"));
        double best = 0;
        bool first = true;
        for (const auto& arg : args) {
            auto v = evaluate(*arg, ctx);
            if (!v.ok())
                return v;
            auto n = v.value().as_number();
            if (!n)
                return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, fn + "() requires numeric args"));
            if (first || (fn == "min" ? *n < best : *n > best)) {
                best = *n;
                first = false;
            }
        }
        return Result<ExprValue>::Ok(ExprValue::make_number(best));
    }
    if (fn == "clamp") {
        if (args.size() != 3)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "clamp() requires 3 args"));
        auto v = evaluate(*args[0], ctx);
        if (!v.ok())
            return v;
        auto lo = evaluate(*args[1], ctx);
        if (!lo.ok())
            return lo;
        auto hi = evaluate(*args[2], ctx);
        if (!hi.ok())
            return hi;
        auto vn = v.value().as_number();
        auto lon = lo.value().as_number();
        auto hin = hi.value().as_number();
        if (!vn || !lon || !hin)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "clamp() requires numeric args"));
        double result = std::clamp(*vn, *lon, *hin);
        return Result<ExprValue>::Ok(ExprValue::make_number(result));
    }
    if (fn == "sqrt") {
        auto v = evaluate(*args[0], ctx);
        if (!v.ok())
            return v;
        auto n = v.value().as_number();
        if (!n)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "sqrt() requires numeric arg"));
        if (*n < 0)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "sqrt() of negative number"));
        return Result<ExprValue>::Ok(ExprValue::make_number(std::sqrt(*n)));
    }
    if (fn == "pow") {
        if (args.size() != 2)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "pow() requires 2 args"));
        auto base = evaluate(*args[0], ctx);
        if (!base.ok())
            return base;
        auto exp = evaluate(*args[1], ctx);
        if (!exp.ok())
            return exp;
        auto bn = base.value().as_number();
        auto en = exp.value().as_number();
        if (!bn || !en)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "pow() requires numeric args"));
        return Result<ExprValue>::Ok(ExprValue::make_number(std::pow(*bn, *en)));
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
        auto cb = cond.value().as_bool();
        if (!cb)
            return Result<ExprValue>::Err(Error::make(ErrorCode::InvalidArgument, "IF condition must be boolean"));
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
