// libFuzzer entry for the restricted expression parser (requirement 13.3): any token
// soup must produce either an AST or a validation error, and evaluating accepted ASTs
// must not crash or read out of bounds.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "streamforge/processing/expression.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 4096)
        return 0; // keep the corpus bounded; the depth limiter covers nesting blowups
    std::string src(reinterpret_cast<const char*>(data), size);
    auto ast = streamforge::expr::parse_expression(src);
    if (ast.ok()) {
        streamforge::expr::EvalContext ctx;
        ctx.lookup_metric = [](const std::string&) { return std::optional<double>(1.0); };
        (void)streamforge::expr::evaluate(*ast.value(), ctx);
    }
    return 0;
}
