#include <qdb/plan/build/aggregate.h>

#include <qdb/plan/build/expr.h>
#include <qdb/plan/build/relation.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/plan_print.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>

namespace NQdb {
namespace NBuild {

namespace NAst = NQumir::NAst;
using NQumir::TError;
using NQumir::TLocation;

namespace {

// Replaces the subtrees equal to a computed GROUP BY key with its column. A
// subquery is no key: it is attached above the aggregate.
NAst::TExprPtr SubstituteKeys(const NAst::TExprPtr& expr, const std::unordered_map<std::string, std::string>& keys) {
    if (!expr || keys.empty()) {
        return expr;
    }
    if (!FindNode(expr, IsSubquery)) {
        if (auto key = keys.find(ExprLine(expr)); key != keys.end()) {
            return Ident(expr->Location, key->second);
        }
    }
    for (auto* child : expr->MutableChildren()) {
        *child = SubstituteKeys(*child, keys);
    }
    return expr;
}

// Groups `input` by the keys and computes the collected aggregates. The
// aggregate executor reads column arguments only and needs a key, so computed
// keys and arguments are materialized in a projection below it and a global
// aggregate groups by a constant `__group__` key. COUNT(DISTINCT x) first
// deduplicates on the keys and x.
std::expected<TOperatorPtr, TError> BuildAggregate(
    TOperatorPtr input,
    const TGrouping& grouping,
    TAggCollector& collector,
    TFreshNames& names,
    const NKernel::TAnnotationContext& annotation)
{
    std::vector<std::string> keys;
    for (const auto& key : grouping.Keys) {
        keys.push_back(key.Name);
    }
    auto specs = collector.TakeSpecs();
    if (auto distinctColumn = collector.DistinctColumn()) {
        if (!grouping.Computed.empty()) {
            return std::unexpected(TError("GROUP BY on an expression with COUNT(DISTINCT) is not supported yet"));
        }
        auto dedupKeys = keys;
        if (std::ranges::find(dedupKeys, *distinctColumn) == dedupKeys.end()) {
            dedupKeys.push_back(*distinctColumn);
        }
        std::vector<TAggregateSpec> innerSpecs;
        std::vector<TAggregateSpec> outerSpecs;
        for (auto& spec : specs) {
            if (collector.IsDistinctSpec(spec.Name)) {
                outerSpecs.push_back(std::move(spec));
                continue;
            }
            if (spec.Arg && !NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg)) {
                return std::unexpected(TError("mixing DISTINCT with a computed aggregate argument is not supported"));
            }
            // Partial aggregates per (keys, x), merged by the outer aggregate.
            auto partial = names.Next("partial");
            const auto merge = spec.Func == "count" ? "sum" : spec.Func;
            innerSpecs.push_back({.Name = partial, .Func = spec.Func, .Arg = std::move(spec.Arg)});
            outerSpecs.push_back({.Name = spec.Name, .Func = merge, .Arg = Ident({}, partial),
                .EmptyIsZero = spec.Func == "count" || spec.EmptyIsZero});
        }
        input = std::make_shared<TAggregateOperator>(std::move(input), std::move(dedupKeys), std::move(innerSpecs));
        specs = std::move(outerSpecs);
    }

    const bool global = keys.empty();
    const bool computedArgs = std::ranges::any_of(specs, [](const TAggregateSpec& spec) {
        return spec.Arg && !NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg);
    });
    if (global || !grouping.Computed.empty() || computedArgs) {
        std::vector<TProjectionSpec> projections;
        std::set<std::string> projected;
        for (const auto& key : grouping.Keys) {
            if (projected.insert(key.Name).second) {
                projections.push_back({.Name = key.Name, .Expression = key.Expression});
            }
        }
        for (auto& spec : specs) {
            if (!spec.Arg) {
                continue;
            }
            auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(spec.Arg);
            const auto column = ident
                ? ident.Cast()->Name
                : std::string();
            // The constant global key keeps its name, so an input column of that
            // name is materialized under another.
            if (ident && !(global && BareColumnName(column) == "__group__")) {
                if (projected.insert(column).second) {
                    projections.push_back({.Name = column, .Expression = spec.Arg});
                }
                continue;
            }
            auto name = names.Next("arg");
            projections.push_back({.Name = name, .Expression = spec.Arg});
            spec.Arg = Ident(spec.Arg->Location, std::move(name));
        }
        if (global) {
            projections.push_back({
                .Name = "__group__",
                .Expression = std::make_shared<NAst::TNumberExpr>(TLocation{}, static_cast<int64_t>(1)),
            });
            keys.push_back("__group__");
        }
        input = std::make_shared<TProjectOperator>(std::move(input), std::move(projections));
        if (auto annotated = Annotate(input, annotation); !annotated) {
            return std::unexpected(annotated.error());
        }
    }
    return std::make_shared<TAggregateOperator>(std::move(input), std::move(keys), std::move(specs), global);
}

// Above the aggregate, expressions read group keys and aggregate results only.
std::expected<void, TError> RequireGroupedColumns(
    const TRelation& from,
    const TOperatorPtr& aggregate,
    const NAst::TExprPtr& expression)
{
    const auto fromColumns = ColumnNames(from.Plan);
    const auto available = ColumnNames(aggregate);
    for (const auto& ident : FindUnboundIdents(expression)) {
        auto column = std::ranges::find(fromColumns, ident->Name);
        if (std::ranges::find(available, ident->Name) != available.end() || column == fromColumns.end()) {
            continue;
        }
        const auto& label = from.Labels[column - fromColumns.begin()];
        return std::unexpected(TError(ident->Location, "column " + (label.empty() ? ident->Name : label)
            + " must appear in GROUP BY or be used in an aggregate function"));
    }
    return {};
}

} // namespace

std::expected<void, TError> Aggregate(
    TBlock& block,
    const TGrouping& grouping,
    const std::vector<NAst::TExprPtr*>& clauses,
    TFreshNames& names,
    const NKernel::TAnnotationContext& annotation)
{
    const TRelation from = block.Tree;
    const auto inputSchema = from.Plan->OutputColumns();
    TAggCollector collector(inputSchema.get(), true, names, annotation);
    for (auto* clause : clauses) {
        auto rewritten = collector.Rewrite(std::move(*clause));
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        *clause = SubstituteKeys(*rewritten, grouping.Computed);
        auto call = FindNode(*clause, [](const NAst::TExprPtr& expr) {
            return HasGroupingCall(expr) && std::ranges::none_of(expr->Children(), HasGroupingCall);
        });
        if (call) {
            return std::unexpected(TError(call->Location, "GROUPING() is not supported by the new builder yet"));
        }
    }
    if (!grouping.Present && collector.Empty()) {
        return {};
    }
    auto aggregate = BuildAggregate(from.Plan, grouping, collector, names, annotation);
    if (!aggregate) {
        return std::unexpected(aggregate.error());
    }
    for (auto* clause : clauses) {
        if (auto grouped = RequireGroupedColumns(from, *aggregate, *clause); !grouped) {
            return grouped;
        }
    }
    // Plain group keys keep their labels: subqueries above the aggregate read
    // them by SQL name.
    block.Tree = CarryLabels(from, std::move(*aggregate));
    block.Ungrouped = from;
    return {};
}

} // namespace NBuild
} // namespace NQdb
