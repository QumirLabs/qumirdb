#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/window.h>
#include <qdb/sql/ast.h>

#include <qumir/error.h>
#include <qumir/parser/ast.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NQdb {

// Lowering of SQL aggregate calls, window functions and grouping onto logical
// operators.

NQumir::NAst::TExprPtr Ident(NQumir::TLocation loc, std::string name);
NQumir::NAst::TExprPtr Call(
    NQumir::TLocation loc, std::string name, std::vector<NQumir::NAst::TExprPtr> args);
std::string ToLower(std::string s);

ESortNulls ConvertSortNulls(NSql::TSqlOrderItem::ENullOrder nulls);

struct TAggCall;

// Key for reusing a plain aggregate slot across SELECT/HAVING (no printer-format
// / separator ambiguity, unlike a concatenated string key).
struct TPlainAggKey {
    std::string Func;
    std::string Arg; // PrintAst(arg), or "*" for count(*)
    bool operator==(const TPlainAggKey&) const = default;
};

struct TPlainAggKeyHash {
    size_t operator()(const TPlainAggKey& key) const {
        size_t result = std::hash<std::string>{}(key.Func);
        result ^= std::hash<std::string>{}(key.Arg)
            + 0x9e3779b9 + (result << 6) + (result >> 2);
        return result;
    }
};

// Generated column names of one aggregation (slots, computed keys, materialized
// arguments, partial aggregates): each differs from the input columns, the SELECT
// aliases and every other generated name, because they share one plan scope.
class TFreshNames {
public:
    void Reserve(std::string name) {
        Taken_.insert(std::move(name));
    }

    // Takes the first `<prefix>_<n>` that is still free.
    std::string Next(const std::string& prefix);

private:
    std::unordered_set<std::string> Taken_;
};

// Replaces aggregate calls in an expression with references to synthetic
// aggregate output columns, collecting the corresponding specs.
class TAggCollector {
public:
    explicit TAggCollector(
        const NQumir::NAst::TStructType* inputSchema,
        bool enableAffineRewrite,
        TFreshNames& names,
        const NKernel::TAnnotationContext& context)
        : InputSchema_(inputSchema)
        , EnableAffineRewrite_(enableAffineRewrite)
        , Names_(names)
        , Context_(context)
    { }
    // The collector keeps a reference to the context.
    TAggCollector(
        const NQumir::NAst::TStructType* inputSchema,
        bool enableAffineRewrite,
        TFreshNames& names,
        const NKernel::TAnnotationContext&& context) = delete;

    std::expected<NQumir::NAst::TExprPtr, NQumir::TError> Rewrite(NQumir::NAst::TExprPtr expr);

    std::vector<TAggregateSpec> TakeSpecs() {
        return std::move(Specs_);
    }

    bool Empty() const {
        return Specs_.empty();
    }

    // Set when a count(distinct <col>) was collected: BuildAggregate realizes it via
    // a double aggregation (inner dedup on the group keys + <col>, then count).
    const std::optional<std::string>& DistinctColumn() const {
        return DistinctColumn_;
    }
    bool HasNonDistinct() const {
        return HasNonDistinct_;
    }

    // Names of the aggregate specs realized as COUNT(DISTINCT); the rest are
    // non-distinct. Used to split into two aggregations when both are present.
    bool IsDistinctSpec(const std::string& name) const {
        return DistinctSpecNames_.count(name) != 0;
    }

    // The aggregate columns collected so far, typed as the aggregate will produce
    // them from the input schema.
    std::expected<std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>>, NQumir::TError>
    SlotTypes() const;

private:
    bool IsIntegerColumn(const NQumir::NAst::TExprPtr& expr) const;
    NQumir::NAst::TExprPtr EmitPlain(
        const std::string& func,
        NQumir::NAst::TExprPtr arg,
        NQumir::TLocation loc);
    std::optional<NQumir::NAst::TExprPtr> TryEmitAffineAggregate(
        const std::string& func,
        const NQumir::NAst::TExprPtr& arg,
        NQumir::TLocation loc);
    std::expected<NQumir::NAst::TTypePtr, NQumir::TError> StddevArgType(
        const TAggCall& agg,
        NQumir::TLocation loc) const;
    std::expected<NQumir::NAst::TExprPtr, NQumir::TError> EmitStddevSamp(
        const TAggCall& agg, NQumir::TLocation loc);
    std::expected<NQumir::NAst::TExprPtr, NQumir::TError> Emit(
        const TAggCall& agg, NQumir::TLocation loc);

    std::vector<TAggregateSpec> Specs_;
    const NQumir::NAst::TStructType* InputSchema_ = nullptr;
    bool EnableAffineRewrite_ = true;
    TFreshNames& Names_;
    std::optional<std::string> DistinctColumn_;
    std::set<std::string> DistinctSpecNames_;
    bool HasNonDistinct_ = false;
    // (Func, PrintAst(Arg)) -> slot name, so the same plain aggregate in SELECT
    // and HAVING reuses one slot instead of emitting count_0/count_1.
    std::unordered_map<TPlainAggKey, std::string, TPlainAggKeyHash> PlainSpecNames_;
    const NKernel::TAnnotationContext& Context_;
};

// Extracts window expressions from projection expressions into a stack of
// TWindowOperator nodes (one per distinct spec) and rewrites each to a reference
// to a synthetic output column. Runs after aggregation, so arguments and
// partition/order keys are already over the aggregated relation.
class TWindowCollector {
    struct TRaw {
        std::string Name;
        std::string Func;
        NQumir::NAst::TExprPtr Arg;
        std::vector<NQumir::NAst::TExprPtr> PartitionExprs;
        std::vector<NSql::TSqlPtr<NSql::TSqlOrderItem>> OrderItems;
        std::optional<TWindowFrame> Frame;
        std::string Signature;
    };

public:
    std::expected<NQumir::NAst::TExprPtr, NQumir::TError> Rewrite(NQumir::NAst::TExprPtr expr);

    bool Empty() const { return Raw_.empty(); }

    std::expected<TOperatorPtr, NQumir::TError> Build(TOperatorPtr input);

private:
    std::expected<NQumir::NAst::TExprPtr, NQumir::TError> Emit(
        const NSql::TSqlPtr<NSql::TWindowExpr>& w);
    std::string Signature(const TRaw& raw) const;

    std::vector<TRaw> Raw_;
    int Counter_ = 0;
};

// A plain column keeps its name; an expression is materialized into a synthetic
// `gb_<n>` column below the aggregate, and references to it are rewritten to that.
struct TGroupKey {
    std::string Name;
    NQumir::NAst::TExprPtr Expression; // Ident(Name) for a plain column
    bool Computed = false;
};

// Drops plain GROUP BY keys determined by another retained key.
void NarrowPlainGroupKeys(
    std::vector<TGroupKey>& keys,
    std::vector<std::vector<size_t>>& sets,
    bool hasGroupingSyntax);

// Whether `expr` is itself an aggregate call. A window's own function also looks
// like one; callers descend into a window through WindowInputs, which leaves it out.
bool IsAggregateCall(const NQumir::NAst::TExprPtr& expr);

// The expressions of a window whose aggregates belong to the window's query level:
// the function's arguments, PARTITION BY and the ORDER BY keys. The function itself
// and the frame bounds are not among them.
std::vector<NQumir::NAst::TExprPtr*> WindowInputs(NSql::TWindowExpr& window);

// Whether `expr` calls an aggregate of its own query level, the calls TAggCollector
// collects: those inside subquery bodies do not count, those inside window
// arguments, PARTITION BY and window ORDER BY do.
bool HasAggregateCall(const NQumir::NAst::TExprPtr& expr);

bool HasGroupingCall(const NQumir::NAst::TExprPtr& expr);

struct TGroupingRewriteInfo {
    bool HasGroupingSyntax = false;
    std::unordered_map<std::string, size_t> KeyIndex;
    std::vector<std::vector<size_t>> Sets;
};

TGroupingRewriteInfo MakeGroupingRewriteInfo(
    const std::vector<TGroupKey>& keys,
    const std::vector<std::vector<size_t>>& sets,
    bool hasGroupingSyntax);

std::expected<NQumir::NAst::TExprPtr, NQumir::TError> RewriteGroupingCalls(
    NQumir::NAst::TExprPtr expr,
    const TGroupingRewriteInfo& info);

// Rewrites subtrees equal to a computed group key into a reference to its column:
// the base columns no longer exist above the aggregate. Matched top-down.
NQumir::NAst::TExprPtr SubstituteGroupKeys(
    NQumir::NAst::TExprPtr expr,
    const std::unordered_map<std::string, std::string>& byExpr);

} // namespace NQdb
