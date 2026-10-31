#include "const_fold.h"

#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/utils/module_path.h>

#include <qumir/error.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/core/printer.h>

#include <bit>
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

bool IsReal(const TTypePtr& type) {
    auto floating = TMaybeType<TFloatType>(type);
    return floating && floating.Cast()->Kind == TFloatType::F32;
}

// A compilation rewrites its expression, so each one gets its own copy. The result is a
// literal of the expression's type. A REAL stays a typed REAL literal, read from the raw
// register: its text form would round it to 15 decimals and make it a DOUBLE.
TExprPtr EvalClosed(
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
        return nullptr;
    }
    if (IsReal(retType)) {
        auto raw = (*compiled)->EvalRaw({});
        if (!raw) {
            return nullptr;
        }
        auto literal = std::make_shared<TNumberExpr>(loc,
            static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(*raw))));
        literal->Type = std::make_shared<TFloatType>(TFloatType::F32);
        return literal;
    }
    auto result = (*compiled)->Eval({});
    return result ? LiteralFromResult(*result) : nullptr;
}

} // namespace

TConstFolder::TConstFolder() = default;

TConstFolder::~TConstFolder() = default;

// Integer, float and boolean arithmetic, comparisons and casts, date
// arithmetic on folded date literals among them, need no qdb module. An
// expression with a qdb type, function or operator does not compile without
// the module and is compiled with it. The module defines operators only for
// its own types and `%` on doubles, which Qumir does not accept, so an
// expression that compiles without the module means the same as with it.
TExprPtr TConstFolder::Eval(const TExprPtr& expr, const TTypePtr& type) {
    if (!Builtin_) {
        Builtin_ = std::make_unique<TVmFrontendContext>();
    }
    if (auto result = EvalClosed(*Builtin_, {}, expr, type)) {
        return result;
    }
    if (!Qumirdb_) {
        Qumirdb_ = std::make_unique<TVmFrontendContext>(
            std::vector<std::string>{NUtils::ModuleFile("qumirdb.oz")});
    }
    return EvalClosed(*Qumirdb_, {"qumirdb"}, expr, type);
}

TExprPtr TConstFolder::Fold(TExprPtr expr) {
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
    auto literal = Eval(rewritten, rewrittenType);
    return literal ? literal : expr;
}

std::shared_ptr<TNumberExpr> TConstFolder::FoldNumber(const TExprPtr& expr) {
    auto [it, inserted] = Numbers_.try_emplace(NCore::PrintAst(expr));
    if (inserted) {
        if (auto folded = TMaybeNode<TNumberExpr>(Fold(expr))) {
            it->second = folded.Cast();
        }
    }
    return it->second;
}

} // namespace NQdb
