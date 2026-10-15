#include "const_fold.h"

#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/error.h>
#include <qumir/parser/ast.h>

#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

constexpr const char* EvalFn = "__const_fold_eval__";

std::optional<std::string> EvalClosed(const TExprPtr& expr, const TTypePtr& retType) {
    NQumir::TLocation loc{};
    auto body = std::make_shared<TBlockExpr>(
        loc, std::vector<TExprPtr>{expr});
    auto compiled = TVmFunction::Compile(QumirdbVmContext(), {
        .NamePrefix = EvalFn,
        .Params = {},
        .Body = std::move(body),
        .ReturnType = retType,
        .Imports = {"qumirdb"},
    });
    if (!compiled) {
        return std::nullopt;
    }
    return (*compiled)->Eval({});
}

// CloneExpr shares types with the original, and compiling resolves named types
// in place. Give the clone its own, so that folding leaves the caller's
// expression as it was.
void DetachNamedTypes(const TExprPtr& expr) {
    if (auto maybeNamed = TMaybeType<TNamedType>(expr->Type)) {
        expr->Type = std::make_shared<TNamedType>(*maybeNamed.Cast());
    }
    for (auto* child : expr->MutableChildren()) {
        if (*child) {
            DetachNamedTypes(*child);
        }
    }
}

TExprPtr LiteralFromResult(const std::string& s) {
    NQumir::TLocation loc{};
    if (s == "true") {
        return std::make_shared<TNumberExpr>(loc, true);
    }
    if (s == "false") {
        return std::make_shared<TNumberExpr>(loc, false);
    }
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    int64_t intValue = 0;
    if (auto [p, ec] = std::from_chars(begin, end, intValue); ec == std::errc{} && p == end) {
        return std::make_shared<TNumberExpr>(loc, intValue);
    }
    double floatValue = 0.0;
    if (auto [p, ec] = std::from_chars(begin, end, floatValue); ec == std::errc{} && p == end) {
        return std::make_shared<TNumberExpr>(loc, floatValue);
    }
    return nullptr;
}

} // namespace

TExprPtr ConstFold(TExprPtr expr) {
    if (!expr || !FindUnboundVars(expr).empty()) {
        return expr;
    }
    // A closed expression can be typed against an empty input schema; expansion
    // rewrites SQL nullability/Decimal and yields the result type used to wrap it.
    // It throws for what it cannot type, such as a decimal product: the caller
    // then keeps the expression, as after any other failure to fold.
    TExprPtr rewritten;
    TTypePtr rewrittenType;
    try {
        auto clone = CloneExpr(expr);
        DetachNamedTypes(clone);
        std::tie(rewritten, rewrittenType) =
            NKernel::ExpandKernelExpr(clone, TStructType({}));
    } catch (const NQumir::TError&) {
        return expr;
    }
    if (!rewrittenType) {
        return expr;
    }
    auto result = EvalClosed(rewritten, rewrittenType);
    if (!result) {
        return expr;
    }
    auto literal = LiteralFromResult(*result);
    return literal ? literal : expr;
}

} // namespace NQdb
