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

// A frontend without source modules. Compiling against the qumirdb module
// composes, checks and lowers all of it again, about 13 ms; an expression over
// Qumir's own types compiles alone in about 0.05 ms.
TVmFrontendContext& BareVmContext() {
    static TVmFrontendContext context;
    return context;
}

TTypePtr DetachedType(const TTypePtr& type) {
    if (auto maybeNamed = TMaybeType<TNamedType>(type)) {
        return std::make_shared<TNamedType>(*maybeNamed.Cast());
    }
    return type;
}

// CloneExpr shares types with the original, and compiling resolves named types
// in place. Give the clone its own, so that folding leaves the caller's
// expression as it was.
void DetachNamedTypes(const TExprPtr& expr) {
    expr->Type = DetachedType(expr->Type);
    for (auto* child : expr->MutableChildren()) {
        if (*child) {
            DetachNamedTypes(*child);
        }
    }
}

// A compilation rewrites its expression, so each one gets its own copy.
std::optional<std::string> EvalClosed(
    TVmFrontendContext& context,
    std::vector<std::string> imports,
    const TExprPtr& expr,
    const TTypePtr& retType)
{
    auto copy = CloneExpr(expr);
    DetachNamedTypes(copy);
    NQumir::TLocation loc{};
    auto body = std::make_shared<TBlockExpr>(
        loc, std::vector<TExprPtr>{std::move(copy)});
    auto compiled = TVmFunction::Compile(context, {
        .NamePrefix = EvalFn,
        .Params = {},
        .Body = std::move(body),
        .ReturnType = DetachedType(retType),
        .Imports = std::move(imports),
    });
    if (!compiled) {
        return std::nullopt;
    }
    return (*compiled)->Eval({});
}

// Integer, float and boolean arithmetic, comparisons and casts, date
// arithmetic on folded date literals among them, need no qdb module. An
// expression with a qdb type, function or operator does not compile without
// the module and is compiled with it. The module defines operators only for
// its own types and `%` on doubles, which Qumir does not accept, so an
// expression that compiles without the module means the same as with it.
std::optional<std::string> EvalClosed(const TExprPtr& expr, const TTypePtr& retType) {
    if (auto result = EvalClosed(BareVmContext(), {}, expr, retType)) {
        return result;
    }
    return EvalClosed(QumirdbVmContext(), {"qumirdb"}, expr, retType);
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
