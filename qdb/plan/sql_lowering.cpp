#include <qdb/plan/sql_lowering.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <utility>

namespace NQdb {

namespace {

namespace NAst = NQumir::NAst;
namespace NSql = NQdb::NSql;
using namespace NAst::NLiterals;
using NQumir::TError;
using NQumir::TLocation;

bool IsAggFunc(const std::string& name) {
    static const std::set<std::string> funcs = {
        "sum", "count", "avg", "min", "max", "stddev_samp"};
    return funcs.count(name) > 0;
}

} // namespace

struct TAggCall {
    std::string Func;
    NAst::TExprPtr Arg; // null for count(*)
    bool Distinct = false;
    bool Star = false;
};

namespace {

std::optional<TAggCall> AsAggCall(const NAst::TExprPtr& expr) {
    auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr);
    if (!call) {
        return std::nullopt;
    }
    auto callee = NAst::TMaybeNode<NAst::TIdentExpr>(call.Cast()->Callee);
    if (!callee) {
        return std::nullopt;
    }
    std::string func = ToLower(callee.Cast()->Name);
    if (!IsAggFunc(func)) {
        return std::nullopt;
    }

    TAggCall agg;
    agg.Func = func;
    const auto& args = call.Cast()->Args;
    if (!args.empty()) {
        const auto& arg = args[0];
        auto star = NAst::TMaybeNode<NAst::TIdentExpr>(arg);
        auto distinct = NAst::TMaybeNode<NAst::TCallExpr>(arg);
        if (star && star.Cast()->Name == "*") {
            agg.Star = true;
        } else if (distinct) {
            auto distinctCallee = NAst::TMaybeNode<NAst::TIdentExpr>(distinct.Cast()->Callee);
            if (distinctCallee && ToLower(distinctCallee.Cast()->Name) == "distinct") {
                agg.Distinct = true;
                const auto& distinctArgs = distinct.Cast()->Args;
                agg.Arg = distinctArgs.empty() ? nullptr : distinctArgs[0];
            } else {
                agg.Arg = arg;
            }
        } else {
            agg.Arg = arg;
        }
    }
    return agg;
}

bool IsDecimalExprHint(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (auto cast = NAst::TMaybeNode<NAst::TCastExpr>(expr)) {
        return IsDecimalType(cast.Cast()->Type);
    }
    return IsDecimalType(expr->Type);
}

NAst::TExprPtr CastF64(TLocation loc, NAst::TExprPtr expr) {
    return std::make_shared<NAst::TCastExpr>(
        std::move(loc), std::move(expr), std::make_shared<NAst::TFloatType>());
}

NAst::TExprPtr F64Literal(TLocation loc, double value) {
    return std::make_shared<NAst::TNumberExpr>(std::move(loc), value);
}

NAst::TExprPtr Binary(
    TLocation loc,
    std::string op,
    NAst::TExprPtr left,
    NAst::TExprPtr right)
{
    return std::make_shared<NAst::TBinaryExpr>(
        std::move(loc), NAst::TOperator(std::move(op)), std::move(left), std::move(right));
}

bool IsStddevSupportedType(const NAst::TTypePtr& type) {
    auto value = NAst::UnwrapNamedType(UnwrapNullableType(type));
    return NAst::TMaybeType<NAst::TIntegerType>(value) ||
        NAst::TMaybeType<NAst::TFloatType>(value);
}

bool IsFloatType(const NAst::TTypePtr& type) {
    return static_cast<bool>(
        NAst::TMaybeType<NAst::TFloatType>(NAst::UnwrapNamedType(UnwrapNullableType(type))));
}

NSql::TSqlPtr<NSql::TSqlOrderItem> CloneSqlOrderItem(
    const NSql::TSqlPtr<NSql::TSqlOrderItem>& item)
{
    if (!item) {
        return nullptr;
    }
    auto copy = std::make_shared<NSql::TSqlOrderItem>();
    copy->Expr = item->Expr ? CloneExpr(item->Expr) : nullptr;
    copy->Desc = item->Desc;
    copy->NullOrder = item->NullOrder;
    return copy;
}

NSql::TSqlPtr<NSql::TSqlOrder> CloneSqlOrder(
    const NSql::TSqlPtr<NSql::TSqlOrder>& order)
{
    if (!order) {
        return nullptr;
    }
    auto copy = std::make_shared<NSql::TSqlOrder>();
    copy->Items.reserve(order->Items.size());
    for (const auto& item : order->Items) {
        copy->Items.push_back(CloneSqlOrderItem(item));
    }
    return copy;
}

NSql::TSqlPtr<NSql::TSqlFrameBound> CloneSqlFrameBound(
    const NSql::TSqlPtr<NSql::TSqlFrameBound>& bound)
{
    if (!bound) {
        return nullptr;
    }
    return std::make_shared<NSql::TSqlFrameBound>(
        bound->Type,
        bound->Expr ? CloneExpr(bound->Expr) : nullptr);
}

NSql::TSqlPtr<NSql::TSqlWindowFrame> CloneSqlWindowFrame(
    const NSql::TSqlPtr<NSql::TSqlWindowFrame>& frame)
{
    if (!frame) {
        return nullptr;
    }
    return std::make_shared<NSql::TSqlWindowFrame>(
        frame->Type,
        CloneSqlFrameBound(frame->Start),
        CloneSqlFrameBound(frame->End));
}

NSql::TSqlPtr<NSql::TSqlWindowSpec> CloneSqlWindowSpec(
    const NSql::TSqlPtr<NSql::TSqlWindowSpec>& spec)
{
    if (!spec) {
        return nullptr;
    }
    return std::make_shared<NSql::TSqlWindowSpec>(
        spec->PartitionBy ? CloneExpr(spec->PartitionBy) : nullptr,
        CloneSqlOrder(spec->OrderBy),
        CloneSqlWindowFrame(spec->Frame));
}

NSql::TSqlPtr<NSql::TWindowExpr> CloneSqlWindowExpr(
    const NSql::TSqlPtr<NSql::TWindowExpr>& window)
{
    if (!window) {
        return nullptr;
    }
    auto copy = std::make_shared<NSql::TWindowExpr>(
        window->Expr ? CloneExpr(window->Expr) : nullptr,
        CloneSqlWindowSpec(window->WindowSpec));
    copy->Type = window->Type;
    return copy;
}

// TODO(#129): Move the affine coefficient arithmetic below to the common VM pipeline
// once planning-time evaluation is fast: ConstFold compiles a fresh TVmFunction per
// call (~4 ms). The move must keep rejecting coefficients that overflow i64, which
// the VM would wrap.
struct TAffineForm {
    NAst::TExprPtr Base;
    int64_t Scale = 1;
    int64_t Offset = 0;
};

std::optional<int64_t> IntegerLiteral(const NAst::TExprPtr& expr) {
    auto maybeNumber = NAst::TMaybeNode<NAst::TNumberExpr>(expr);
    if (!maybeNumber) {
        return std::nullopt;
    }
    const auto number = maybeNumber.Cast();
    if (number->IsFloat()) {
        return std::nullopt;
    }
    return number->IntValue;
}

std::optional<int64_t> AddExact(int64_t left, int64_t right) {
    int64_t result;
    return __builtin_add_overflow(left, right, &result)
        ? std::nullopt
        : std::optional<int64_t>(result);
}

std::optional<int64_t> SubExact(int64_t left, int64_t right) {
    int64_t result;
    return __builtin_sub_overflow(left, right, &result)
        ? std::nullopt
        : std::optional<int64_t>(result);
}

std::optional<int64_t> MulExact(int64_t left, int64_t right) {
    int64_t result;
    return __builtin_mul_overflow(left, right, &result)
        ? std::nullopt
        : std::optional<int64_t>(result);
}

// Recognize a single column combined with integer literals through +, -, and
// *. Restricting the leaf to a column avoids duplicating evaluation of a
// potentially volatile computed expression when SUM(base) and COUNT(base)
// are both emitted.
std::optional<TAffineForm> ExtractAffine(const NAst::TExprPtr& expr) {
    if (NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
        return TAffineForm{.Base = expr};
    }
    auto maybeBinary = NAst::TMaybeNode<NAst::TBinaryExpr>(expr);
    if (!maybeBinary) {
        return std::nullopt;
    }
    const auto binary = maybeBinary.Cast();

    const auto leftConstant = IntegerLiteral(binary->Left);
    const auto rightConstant = IntegerLiteral(binary->Right);
    if (leftConstant.has_value() == rightConstant.has_value()) {
        return std::nullopt;
    }
    const bool constantOnLeft = leftConstant.has_value();
    const int64_t constant = constantOnLeft ? *leftConstant : *rightConstant;
    auto affine = ExtractAffine(constantOnLeft ? binary->Right : binary->Left);
    if (!affine) {
        return std::nullopt;
    }

    if (binary->Operator == "+"_op) {
        auto offset = AddExact(affine->Offset, constant);
        if (!offset) {
            return std::nullopt;
        }
        affine->Offset = *offset;
        return affine;
    }
    if (binary->Operator == "-"_op) {
        auto scale = constantOnLeft
            ? SubExact(0, affine->Scale)
            : std::optional<int64_t>(affine->Scale);
        auto offset = constantOnLeft
            ? SubExact(constant, affine->Offset)
            : SubExact(affine->Offset, constant);
        if (!scale || !offset) {
            return std::nullopt;
        }
        affine->Scale = *scale;
        affine->Offset = *offset;
        return affine;
    }
    if (binary->Operator == "*"_op) {
        auto scale = MulExact(affine->Scale, constant);
        auto offset = MulExact(affine->Offset, constant);
        if (!scale || !offset) {
            return std::nullopt;
        }
        affine->Scale = *scale;
        affine->Offset = *offset;
        return affine;
    }
    return std::nullopt;
}

std::optional<TWindowFrame> ConvertFrame(
    const NSql::TSqlPtr<NSql::TSqlWindowFrame>& frame)
{
    if (!frame) {
        return std::nullopt;
    }
    auto convertBound = [](const NSql::TSqlPtr<NSql::TSqlFrameBound>& bound) -> TFrameBound {
        TFrameBound out;
        if (!bound) {
            return out;
        }
        switch (bound->Type) {
            case NSql::TSqlFrameBound::EType::UnboundedPreceding:
                out.Kind = EFrameBoundKind::UnboundedPreceding; break;
            case NSql::TSqlFrameBound::EType::Preceding:
                out.Kind = EFrameBoundKind::Preceding; out.Offset = bound->Expr; break;
            case NSql::TSqlFrameBound::EType::CurrentRow:
                out.Kind = EFrameBoundKind::CurrentRow; break;
            case NSql::TSqlFrameBound::EType::Following:
                out.Kind = EFrameBoundKind::Following; out.Offset = bound->Expr; break;
            case NSql::TSqlFrameBound::EType::UnboundedFollowing:
                out.Kind = EFrameBoundKind::UnboundedFollowing; break;
        }
        return out;
    };
    TWindowFrame out;
    out.Mode = frame->Type == NSql::TSqlWindowFrame::EType::Rows
        ? EWindowFrameMode::Rows : EWindowFrameMode::Range;
    out.Start = convertBound(frame->Start);
    out.End = frame->End ? convertBound(frame->End) : TFrameBound{};
    return out;
}

std::string FrameSignature(const std::optional<TWindowFrame>& frame) {
    if (!frame) {
        return "none";
    }
    auto boundSig = [](const TFrameBound& bound) {
        std::string s(FrameBoundKindName(bound.Kind));
        if (bound.Offset) {
            s += ":" + NAst::NCore::PrintAst(bound.Offset);
        }
        return s;
    };
    return std::string(WindowFrameModeName(frame->Mode)) + "[" +
        boundSig(frame->Start) + "," + boundSig(frame->End) + "]";
}

// Returns the base column for the deliberately small class of deterministic
// expressions that we can reconstruct above an aggregate.  If that base is
// itself a group key, the expression cannot split one of its groups and is
// therefore redundant in the physical hash key.
std::optional<std::string> AffineGroupKeyBase(const NAst::TExprPtr& expr) {
    if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
        return ident.Cast()->Name;
    }
    auto maybeBinary = NAst::TMaybeNode<NAst::TBinaryExpr>(expr);
    if (!maybeBinary) {
        return std::nullopt;
    }
    const auto binary = maybeBinary.Cast();
    if (binary->Operator != "+"_op && binary->Operator != "-"_op && binary->Operator != "*"_op) {
        return std::nullopt;
    }
    const bool leftConstant = IntegerLiteral(binary->Left).has_value();
    const bool rightConstant = IntegerLiteral(binary->Right).has_value();
    if (leftConstant == rightConstant) {
        return std::nullopt;
    }
    return AffineGroupKeyBase(leftConstant ? binary->Right : binary->Left);
}

bool IsLiteralGroupKey(const NAst::TExprPtr& expr) {
    // In the current qdb dialect GROUP BY 1 is the literal 1, not a SELECT-list
    // ordinal.  If ordinals are added, they must be resolved before this check.
    if (NAst::TMaybeNode<NAst::TNumberExpr>(expr) ||
        NAst::TMaybeNode<NAst::TStringLiteralExpr>(expr))
    {
        return true;
    }
    if (auto cast = NAst::TMaybeNode<NAst::TCastExpr>(expr)) {
        return IsLiteralGroupKey(cast.Cast()->Operand);
    }
    return false;
}

bool IsGroupingCall(const NAst::TExprPtr& expr) {
    auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr);
    if (!call) {
        return false;
    }
    auto callee = NAst::TMaybeNode<NAst::TIdentExpr>(call.Cast()->Callee);
    return callee && ToLower(callee.Cast()->Name) == "grouping";
}

NAst::TExprPtr I64Literal(TLocation loc, int64_t value) {
    return std::make_shared<NAst::TNumberExpr>(std::move(loc), value);
}

NAst::TExprPtr I32Literal(TLocation loc, int32_t value) {
    auto ret = std::make_shared<NAst::TNumberExpr>(std::move(loc), static_cast<int64_t>(value));
    ret->Type = std::make_shared<NAst::TIntegerType>(NAst::TIntegerType::I32);
    return ret;
}

bool GroupingSetContainsKey(const std::vector<size_t>& set, size_t keyIndex) {
    return std::find(set.begin(), set.end(), keyIndex) != set.end();
}

std::expected<NAst::TExprPtr, TError> BuildGroupingValue(
    const NAst::TExprPtr& expr,
    const TGroupingRewriteInfo& info)
{
    auto call = NAst::TMaybeNode<NAst::TCallExpr>(expr);
    if (!call || call.Cast()->Args.size() != 1) {
        return std::unexpected(TError(expr->Location, "GROUPING() expects exactly one argument"));
    }
    if (!info.HasGroupingSyntax) {
        return std::unexpected(TError(expr->Location,
            "GROUPING() requires GROUPING SETS, ROLLUP, or CUBE"));
    }

    auto arg = call.Cast()->Args[0];
    auto key = info.KeyIndex.find(NAst::NCore::PrintAst(arg));
    if (key == info.KeyIndex.end()) {
        return std::unexpected(TError(arg->Location,
            "GROUPING() argument must be a GROUP BY key"));
    }

    NAst::TExprPtr result = I64Literal(expr->Location, 0);
    for (size_t setId = info.Sets.size(); setId-- > 0;) {
        if (GroupingSetContainsKey(info.Sets[setId], key->second)) {
            continue;
        }
        auto condition = std::make_shared<NAst::TBinaryExpr>(
            expr->Location,
            NAst::TOperator("=="),
            Ident(expr->Location, "__grouping_id__"),
            I32Literal(expr->Location, static_cast<int32_t>(setId)));
        result = std::make_shared<NAst::TIfExpr>(
            expr->Location,
            std::move(condition),
            I64Literal(expr->Location, 1),
            std::move(result));
    }
    return result;
}

} // namespace

NAst::TExprPtr Ident(TLocation loc, std::string name) {
    return std::make_shared<NAst::TIdentExpr>(std::move(loc), std::move(name));
}

NAst::TExprPtr Call(TLocation loc, std::string name, std::vector<NAst::TExprPtr> args) {
    return std::make_shared<NAst::TCallExpr>(
        loc, Ident(loc, std::move(name)), std::move(args));
}

std::string ToLower(std::string s) {
    for (char& c : s) {
        c = std::tolower(static_cast<unsigned char>(c));
    }
    return s;
}

std::string BareColumnName(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos
        ? name
        : name.substr(dot + 1);
}

ESortNulls ConvertSortNulls(NSql::TSqlOrderItem::ENullOrder nulls) {
    switch (nulls) {
        case NSql::TSqlOrderItem::ENullOrder::Default:
            return ESortNulls::Default;
        case NSql::TSqlOrderItem::ENullOrder::First:
            return ESortNulls::First;
        case NSql::TSqlOrderItem::ENullOrder::Last:
            return ESortNulls::Last;
    }
    return ESortNulls::Default;
}

std::expected<NAst::TExprPtr, TError> TAggCollector::Rewrite(NAst::TExprPtr expr) {
    if (!expr) {
        return expr;
    }
    // A window function evaluates after aggregation: keep the window node in
    // place, but hoist any aggregates from its arguments and PARTITION/ORDER
    // (e.g. `sum(sum(x)) over (...)`, `rank() over (order by sum(x))`).
    if (auto window = NAst::TMaybeNode<NSql::TWindowExpr>(expr)) {
        auto w = CloneSqlWindowExpr(window.Cast());
        if (auto call = NAst::TMaybeNode<NAst::TCallExpr>(w->Expr)) {
            for (auto& arg : call.Cast()->Args) {
                auto rewritten = Rewrite(arg);
                if (!rewritten) {
                    return std::unexpected(rewritten.error());
                }
                arg = std::move(*rewritten);
            }
        }
        if (w->WindowSpec) {
            if (w->WindowSpec->PartitionBy) {
                auto rewritten = Rewrite(w->WindowSpec->PartitionBy);
                if (!rewritten) {
                    return std::unexpected(rewritten.error());
                }
                w->WindowSpec->PartitionBy = std::move(*rewritten);
            }
            if (w->WindowSpec->OrderBy) {
                for (auto& item : w->WindowSpec->OrderBy->Items) {
                    if (!item || !item->Expr) {
                        continue;
                    }
                    auto rewritten = Rewrite(item->Expr);
                    if (!rewritten) {
                        return std::unexpected(rewritten.error());
                    }
                    item->Expr = std::move(*rewritten);
                }
            }
        }
        return w;
    }
    if (auto agg = AsAggCall(expr)) {
        return Emit(*agg, expr->Location);
    }
    for (auto* child : expr->MutableChildren()) {
        auto rewritten = Rewrite(*child);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        *child = std::move(*rewritten);
    }
    return expr;
}

bool TAggCollector::IsIntegerColumn(const NAst::TExprPtr& expr) const {
    auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(expr);
    if (!ident || !InputSchema_) {
        return false;
    }
    NAst::TTypePtr type;
    for (const auto& [name, candidate] : InputSchema_->Fields) {
        if (name == ident.Cast()->Name) {
            type = candidate;
            break;
        }
    }
    type = NAst::UnwrapNamedType(UnwrapNullableType(type));
    return static_cast<bool>(NAst::TMaybeType<NAst::TIntegerType>(type));
}

NAst::TExprPtr TAggCollector::EmitPlain(
    const std::string& func,
    NAst::TExprPtr arg,
    TLocation loc)
{
    TPlainAggKey key{
        .Func = func,
        .Arg = arg ? NAst::NCore::PrintAst(arg) : std::string("*"),
    };
    if (auto it = PlainSpecNames_.find(key); it != PlainSpecNames_.end()) {
        return Ident(std::move(loc), it->second);
    }
    std::string name = NextName(func);
    Specs_.push_back({ .Name = name, .Func = func, .Arg = std::move(arg) });
    PlainSpecNames_.emplace(std::move(key), name);
    return Ident(std::move(loc), std::move(name));
}

std::optional<NAst::TExprPtr> TAggCollector::TryEmitAffineAggregate(
    const std::string& func,
    const NAst::TExprPtr& arg,
    TLocation loc)
{
    if (!EnableAffineRewrite_) {
        return std::nullopt;
    }
    auto affine = ExtractAffine(arg);
    if (!affine || !IsIntegerColumn(affine->Base)) {
        return std::nullopt;
    }

    if (func == "count") {
        return EmitPlain("count", CloneExpr(affine->Base), std::move(loc));
    }

    std::string reducer = func;
    if ((func == "min" || func == "max") && affine->Scale < 0) {
        reducer = func == "min" ? "max" : "min";
    } else if (func == "avg") {
        reducer = "sum";
    }
    auto value = EmitPlain(reducer, CloneExpr(affine->Base), loc);
    if (affine->Scale != 1) {
        value = Binary(
            loc,
            "*",
            std::make_shared<NAst::TNumberExpr>(loc, affine->Scale),
            std::move(value));
    }

    if (func == "sum" || func == "avg") {
        NAst::TExprPtr count;
        if (func == "avg" || affine->Offset != 0) {
            count = EmitPlain("count", CloneExpr(affine->Base), loc);
        }
        if (affine->Offset != 0) {
            auto contribution = Binary(
                loc,
                "*",
                std::make_shared<NAst::TNumberExpr>(loc, affine->Offset),
                CloneExpr(count));
            value = Binary(loc, "+", std::move(value), std::move(contribution));
        }
        if (func == "avg") {
            auto divisor = std::make_shared<NAst::TCastExpr>(
                loc, std::move(count), std::make_shared<NAst::TFloatType>());
            value = Binary(loc, "/", std::move(value), std::move(divisor));
        }
    } else if (affine->Offset != 0) {
        value = Binary(
            loc,
            "+",
            std::move(value),
            std::make_shared<NAst::TNumberExpr>(loc, affine->Offset));
    }
    return value;
}

std::expected<NAst::TTypePtr, TError> TAggCollector::StddevArgType(
    const TAggCall& agg,
    TLocation loc) const
{
    if (!agg.Arg || agg.Star) {
        return std::unexpected(TError(loc, "stddev_samp requires one argument"));
    }
    if (IsDecimalExprHint(agg.Arg)) {
        return std::unexpected(TError(loc, "stddev_samp(decimal) is not supported in qdb v1"));
    }
    if (auto cast = NAst::TMaybeNode<NAst::TCastExpr>(agg.Arg);
        cast && (!InputSchema_ || InputSchema_->Fields.empty()))
    {
        if (!IsStddevSupportedType(cast.Cast()->Type)) {
            return std::unexpected(TError(loc,
                "stddev_samp argument must be integer or f64 in qdb v1"));
        }
        return cast.Cast()->Type;
    }
    if (!InputSchema_ || InputSchema_->Fields.empty()) {
        return NAst::TTypePtr{};
    }

    NAst::TTypePtr type;
    try {
        type = NKernel::AnnotateExprType(agg.Arg, *InputSchema_, &Context_);
    } catch (const TError& error) {
        return std::unexpected(error);
    }
    if (IsDecimalType(type)) {
        return std::unexpected(TError(loc, "stddev_samp(decimal) is not supported in qdb v1"));
    }
    if (!IsStddevSupportedType(type)) {
        return std::unexpected(TError(loc,
            "stddev_samp argument must be integer or f64 in qdb v1"));
    }
    return type;
}

std::expected<NAst::TExprPtr, TError> TAggCollector::EmitStddevSamp(const TAggCall& agg, TLocation loc) {
    auto type = StddevArgType(agg, loc);
    if (!type) {
        return std::unexpected(type.error());
    }

    const bool needsCast = *type && !IsFloatType(*type);
    auto valueArg = [&](TLocation argLoc) -> NAst::TExprPtr {
        auto arg = CloneExpr(agg.Arg);
        return needsCast ? CastF64(std::move(argLoc), std::move(arg)) : arg;
    };

    std::string sumName = NextName("sum");
    Specs_.push_back({
        .Name = sumName,
        .Func = "sum",
        .Arg = valueArg(loc),
    });

    std::string sumSqName = NextName("sum");
    auto squared = Binary(loc, "*", valueArg(loc), valueArg(loc));
    Specs_.push_back({
        .Name = sumSqName,
        .Func = "sum",
        .Arg = std::move(squared),
    });

    std::string countName = NextName("count");
    Specs_.push_back({
        .Name = countName,
        .Func = "count",
        .Arg = CloneExpr(agg.Arg),
    });

    auto count = Ident(loc, countName);
    auto countF = CastF64(loc, Ident(loc, countName));
    auto numerator = Binary(loc, "-",
        Ident(loc, sumSqName),
        Binary(loc, "/",
            Binary(loc, "*", Ident(loc, sumName), Ident(loc, sumName)),
            CastF64(loc, Ident(loc, countName))));
    auto variance = Binary(loc, "/",
        std::move(numerator),
        Binary(loc, "-", std::move(countF), F64Literal(loc, 1.0)));
    auto stddev = Call(loc, "qdb_sqrt", {std::move(variance)});
    return std::make_shared<NAst::TIfExpr>(
        loc,
        Binary(loc, "<", std::move(count), std::make_shared<NAst::TNumberExpr>(loc, int64_t{2})),
        Call(loc, "qdb_sql_null", {}),
        std::move(stddev));
}

std::expected<NAst::TExprPtr, TError> TAggCollector::Emit(const TAggCall& agg, TLocation loc) {
    if (agg.Distinct) {
        if (agg.Func != "count") {
            return std::unexpected(TError(loc, "only count(distinct ...) is supported"));
        }
        auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(agg.Arg);
        if (!ident) {
            return std::unexpected(TError(loc, "count(distinct ...) requires a column argument"));
        }
        const std::string& column = ident.Cast()->Name;
        if (DistinctColumn_ && *DistinctColumn_ != column) {
            return std::unexpected(TError(loc, "multiple distinct columns are not supported"));
        }
        DistinctColumn_ = column;
        std::string name = NextName("count");
        DistinctSpecNames_.insert(name);
        Specs_.push_back({ .Name = name, .Func = "count", .Arg = agg.Arg });
        return Ident(loc, name);
    }

    HasNonDistinct_ = true;

    if (agg.Func == "stddev_samp") {
        return EmitStddevSamp(agg, loc);
    }

    NAst::TExprPtr arg = (agg.Func == "count" && agg.Star)
        ? nullptr
        : agg.Arg;
    if (arg && (agg.Func == "sum" || agg.Func == "avg" ||
                agg.Func == "min" || agg.Func == "max" ||
                agg.Func == "count"))
    {
        if (auto rewritten = TryEmitAffineAggregate(agg.Func, arg, loc)) {
            return std::move(*rewritten);
        }
    }

    // avg has no aggregate kernel; express it as sum / count. The count is over the
    // argument (not count(*)): SQL avg(x) ignores rows where x IS NULL.
    if (agg.Func == "avg") {
        std::string sumName = NextName("sum");
        Specs_.push_back({ .Name = sumName, .Func = "sum", .Arg = agg.Arg });
        std::string countName = NextName("count");
        Specs_.push_back({ .Name = countName, .Func = "count", .Arg = CloneExpr(agg.Arg) });

        NAst::TExprPtr count = Ident(loc, countName);
        if (!IsDecimalExprHint(agg.Arg)) {
            count = std::make_shared<NAst::TCastExpr>(
                loc, std::move(count), std::make_shared<NAst::TFloatType>());
        }
        return std::make_shared<NAst::TBinaryExpr>(
            loc, NAst::TOperator('/'), Ident(loc, sumName), std::move(count));
    }

    return EmitPlain(agg.Func, std::move(arg), std::move(loc));
}

std::string TAggCollector::NextName(const std::string& func) {
    // Slots share the aggregate output with the plain group keys, which are
    // input columns, so a slot cannot take an input column's name.
    auto isInputColumn = [&](const std::string& name) {
        return InputSchema_ && std::ranges::any_of(InputSchema_->Fields, [&](const auto& field) {
            return BareColumnName(field.first) == name;
        });
    };
    std::string name;
    do {
        name = func + "_" + std::to_string(Counter_++);
    } while (isInputColumn(name));
    return name;
}

std::expected<NAst::TExprPtr, TError> TWindowCollector::Rewrite(NAst::TExprPtr expr) {
    if (!expr) {
        return expr;
    }
    if (auto window = NAst::TMaybeNode<NSql::TWindowExpr>(expr)) {
        return Emit(window.Cast());
    }
    for (auto* child : expr->MutableChildren()) {
        auto rewritten = Rewrite(*child);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        *child = std::move(*rewritten);
    }
    return expr;
}

std::expected<TOperatorPtr, TError> TWindowCollector::Build(TOperatorPtr input) {
    // Plain idents keep their name; a computed key/arg is materialized into a
    // synthetic column in one pass-through project below the whole stack.
    std::map<std::string, std::pair<std::string, NAst::TExprPtr>> computed;
    auto keyName = [&](const NAst::TExprPtr& expr) -> std::string {
        if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
            return ident.Cast()->Name;
        }
        auto printed = NAst::NCore::PrintAst(expr);
        auto it = computed.find(printed);
        if (it == computed.end()) {
            std::string name = "wk_" + std::to_string(computed.size());
            computed.emplace(printed, std::make_pair(name, expr));
            return name;
        }
        return it->second.first;
    };

    struct TResolved {
        std::vector<std::string> PartitionKeys;
        std::vector<TSortKey> OrderKeys;
        std::optional<TWindowFrame> Frame;
        std::string Signature;
        TWindowFunc Func;
    };
    std::vector<TResolved> resolved;
    resolved.reserve(Raw_.size());
    for (auto& raw : Raw_) {
        TResolved r;
        for (const auto& expr : raw.PartitionExprs) {
            r.PartitionKeys.push_back(keyName(expr));
        }
        for (const auto& item : raw.OrderItems) {
            r.OrderKeys.push_back(TSortKey{
                .Column = keyName(item->Expr),
                .Direction = item->Desc ? ESortDirection::Desc : ESortDirection::Asc,
                .Nulls = ConvertSortNulls(item->NullOrder),
            });
        }
        r.Frame = raw.Frame;
        r.Signature = raw.Signature;
        r.Func = TWindowFunc{
            .Name = raw.Name,
            .Func = raw.Func,
            .Arg = raw.Arg ? Ident({}, keyName(raw.Arg)) : nullptr,
        };
        resolved.push_back(std::move(r));
    }

    TOperatorPtr node = std::move(input);
    if (!computed.empty()) {
        auto&& schema = node->OutputColumns();
        std::vector<TProjectionSpec> projs;
        if (schema) {
            for (const auto& [name, _] : schema->Fields) {
                projs.push_back({ .Name = name, .Expression = Ident({}, name) });
            }
        }
        for (const auto& [_, entry] : computed) {
            projs.push_back({ .Name = entry.first, .Expression = entry.second });
        }
        node = std::make_shared<TProjectOperator>(std::move(node), std::move(projs));
    }

    // One node per distinct spec, in first-seen order.
    std::map<std::string, size_t> sigToGroup;
    std::vector<std::vector<size_t>> groups;
    for (size_t i = 0; i < resolved.size(); ++i) {
        auto [it, inserted] = sigToGroup.try_emplace(resolved[i].Signature, groups.size());
        if (inserted) {
            groups.emplace_back();
        }
        groups[it->second].push_back(i);
    }
    for (const auto& group : groups) {
        const auto& head = resolved[group[0]];
        std::vector<TWindowFunc> funcs;
        funcs.reserve(group.size());
        for (auto idx : group) {
            funcs.push_back(std::move(resolved[idx].Func));
        }
        node = std::make_shared<TWindowOperator>(
            std::move(node), head.PartitionKeys, head.OrderKeys, head.Frame,
            std::move(funcs));
    }
    return node;
}

std::expected<NAst::TExprPtr, TError> TWindowCollector::Emit(const NSql::TSqlPtr<NSql::TWindowExpr>& w) {
    auto call = NAst::TMaybeNode<NAst::TCallExpr>(w->Expr);
    if (!call) {
        return std::unexpected(TError(w->Location, "unsupported window function expression"));
    }
    auto callee = NAst::TMaybeNode<NAst::TIdentExpr>(call.Cast()->Callee);
    if (!callee) {
        return std::unexpected(TError(w->Location, "unsupported window function"));
    }
    std::string func = ToLower(callee.Cast()->Name);
    if (func != "rank" && func != "sum" && func != "avg" && func != "max") {
        return std::unexpected(TError(w->Location,
            "window function '" + func + "' is not supported"));
    }
    TRaw raw;
    raw.Func = func;
    const auto& args = call.Cast()->Args;
    if (!args.empty()) {
        raw.Arg = args[0];
    }
    if (func != "rank" && !raw.Arg) {
        return std::unexpected(TError(w->Location, func + " window requires an argument"));
    }
    if (w->WindowSpec) {
        if (w->WindowSpec->PartitionBy) {
            if (auto block = NAst::TMaybeNode<NAst::TBlockExpr>(w->WindowSpec->PartitionBy)) {
                for (const auto& expr : block.Cast()->Stmts) {
                    raw.PartitionExprs.push_back(expr);
                }
            } else {
                raw.PartitionExprs.push_back(w->WindowSpec->PartitionBy);
            }
        }
        if (w->WindowSpec->OrderBy) {
            for (const auto& item : w->WindowSpec->OrderBy->Items) {
                if (item && item->Expr) {
                    raw.OrderItems.push_back(item);
                }
            }
        }
        raw.Frame = ConvertFrame(w->WindowSpec->Frame);
    }
    raw.Name = "w_" + std::to_string(Counter_++);
    raw.Signature = Signature(raw);
    auto loc = w->Location;
    Raw_.push_back(std::move(raw));
    return Ident(loc, Raw_.back().Name);
}

std::string TWindowCollector::Signature(const TRaw& raw) const {
    std::string s = "p:";
    for (const auto& expr : raw.PartitionExprs) {
        s += NAst::NCore::PrintAst(expr) + ";";
    }
    s += "o:";
    for (const auto& item : raw.OrderItems) {
        s += NAst::NCore::PrintAst(item->Expr);
        s += item->Desc ? " desc " : " asc ";
        s += std::to_string(static_cast<int>(item->NullOrder));
        s += ";";
    }
    s += "f:" + FrameSignature(raw.Frame);
    return s;
}

// This normalization intentionally lives in the SQL builder rather than a
// later optimizer pass: it must run after GROUP BY aliases are resolved but
// before a computed expression is hidden behind its materialized gb_n column.
// Removed expressions remain in SELECT/HAVING and are evaluated once per output
// group from the retained base key.  Grouping sets are intentionally excluded:
// their individual key subsets and GROUPING() bit positions are observable SQL
// state.
void NarrowPlainGroupKeys(
    std::vector<TGroupKey>& keys,
    std::vector<std::vector<size_t>>& sets,
    bool hasGroupingSyntax)
{
    if (hasGroupingSyntax || keys.size() < 2) {
        return;
    }

    std::unordered_set<std::string> directKeys;
    for (const auto& key : keys) {
        if (!key.Computed) {
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(key.Expression)) {
                directKeys.insert(ident.Cast()->Name);
            }
        }
    }

    std::vector<TGroupKey> narrowed;
    narrowed.reserve(keys.size());
    for (auto& key : keys) {
        bool redundant = IsLiteralGroupKey(key.Expression);
        if (!redundant && key.Computed) {
            if (auto base = AffineGroupKeyBase(key.Expression)) {
                redundant = directKeys.contains(*base);
            }
        }
        if (!redundant) {
            narrowed.push_back(std::move(key));
        }
    }

    // Keep the grouped-empty-input behavior distinct from a global aggregate.
    if (narrowed.empty()) {
        return;
    }
    keys = std::move(narrowed);
    sets.assign(1, {});
    sets.front().reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        sets.front().push_back(i);
    }
}

bool HasGroupingCall(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (IsGroupingCall(expr)) {
        return true;
    }
    for (const auto& child : expr->Children()) {
        if (HasGroupingCall(child)) {
            return true;
        }
    }
    return false;
}

TGroupingRewriteInfo MakeGroupingRewriteInfo(
    const std::vector<TGroupKey>& keys,
    const std::vector<std::vector<size_t>>& sets,
    bool hasGroupingSyntax)
{
    TGroupingRewriteInfo info;
    info.HasGroupingSyntax = hasGroupingSyntax;
    info.Sets = sets;
    for (size_t i = 0; i < keys.size(); ++i) {
        info.KeyIndex.emplace(NAst::NCore::PrintAst(keys[i].Expression), i);
        info.KeyIndex.emplace(NAst::NCore::PrintAst(Ident({}, keys[i].Name)), i);
    }
    return info;
}

std::expected<NAst::TExprPtr, TError> RewriteGroupingCalls(
    NAst::TExprPtr expr,
    const TGroupingRewriteInfo& info)
{
    if (!expr) {
        return expr;
    }
    if (IsGroupingCall(expr)) {
        return BuildGroupingValue(expr, info);
    }
    for (auto* child : expr->MutableChildren()) {
        auto rewritten = RewriteGroupingCalls(*child, info);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        *child = std::move(*rewritten);
    }
    return expr;
}

NAst::TExprPtr SubstituteGroupKeys(
    NAst::TExprPtr expr,
    const std::unordered_map<std::string, std::string>& byExpr)
{
    if (!expr) {
        return expr;
    }
    if (auto it = byExpr.find(NAst::NCore::PrintAst(expr)); it != byExpr.end()) {
        return Ident(expr->Location, it->second);
    }
    for (auto* child : expr->MutableChildren()) {
        *child = SubstituteGroupKeys(*child, byExpr);
    }
    return expr;
}

} // namespace NQdb
