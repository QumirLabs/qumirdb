#include <qdb/plan/plan_print.h>

#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/window.h>

#include <qumir/parser/core/printer.h>

#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace NQdb {

std::string ExprLine(const NQumir::NAst::TExprPtr& expr) {
    if (!expr) {
        return "";
    }
    return NQumir::NAst::NCore::PrintAst(
        expr,
        NQumir::NAst::NCore::TPrintOptions{.Pretty = false});
}

std::string JoinPlanLabel(const TJoinOperator& join) {
    std::string label = "join " + std::string(JoinTypeName(join.JoinType()));
    const auto& keys = join.Keys();
    for (size_t i = 0; i < keys.size(); ++i) {
        label += (i ? ", " : " [") + keys[i].Left
            + (keys[i].NullsEqual ? " IS NOT DISTINCT FROM " : " = ") + keys[i].Right;
    }
    if (!keys.empty()) {
        label += "]";
    }
    if (const auto& runtimeFilter = join.RuntimeFilter()) {
        label += " emit-filter " + std::to_string(runtimeFilter->Id) + " from "
            + std::string(JoinFilterSideName(runtimeFilter->BuildSide));
    }
    if (join.Filter()) {
        label += " residual " + ExprLine(join.Filter());
    }
    return label;
}

std::string AggregatePlanLabel(const TAggregateOperator& aggregate) {
    std::string label = "aggregate";
    const auto& keys = aggregate.GroupKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        label += (i ? ", " : " keys=[") + keys[i];
    }
    if (!keys.empty()) {
        label += "]";
    }
    const auto& aggs = aggregate.Aggs();
    for (size_t i = 0; i < aggs.size(); ++i) {
        label += (i ? ", " : " aggs=[") + aggs[i].Name + "=" + aggs[i].Func;
    }
    if (!aggs.empty()) {
        label += "]";
    }
    return label;
}

namespace {

std::string FormatEstimate(double value) {
    if (!std::isfinite(value)) {
        return "inf";
    }
    const double abs = std::fabs(value);
    const char* suffix = "";
    double scaled = value;
    if (abs >= 1e12) {
        scaled = value / 1e12;
        suffix = "T";
    } else if (abs >= 1e9) {
        scaled = value / 1e9;
        suffix = "B";
    } else if (abs >= 1e6) {
        scaled = value / 1e6;
        suffix = "M";
    } else if (abs >= 1e3) {
        scaled = value / 1e3;
        suffix = "K";
    }

    std::ostringstream s;
    if (std::fabs(scaled) >= 100.0 || std::fabs(scaled - std::round(scaled)) < 0.05) {
        s << std::fixed << std::setprecision(0) << scaled;
    } else {
        s << std::fixed << std::setprecision(1) << scaled;
    }
    s << suffix;
    return s.str();
}

std::string SortKeyLabel(const TSortKey& key) {
    std::string label = key.Column;
    label += " " + std::string(SortDirectionName(key.Direction));
    if (key.Nulls != ESortNulls::Default) {
        label += " " + std::string(SortNullsName(key.Nulls));
    }
    return label;
}

} // namespace

std::string SortPlanLabel(
    std::string_view kind,
    const std::vector<TSortKey>& keys,
    std::optional<int64_t> limit)
{
    std::string label(kind);
    for (size_t i = 0; i < keys.size(); ++i) {
        label += (i ? ", " : " [") + SortKeyLabel(keys[i]);
    }
    if (!keys.empty()) {
        label += "]";
    }
    if (limit) {
        label += " limit " + std::to_string(*limit);
    }
    return label;
}

std::string WindowPlanLabel(const TWindowOperator& window) {
    std::string label = "window";
    const auto& parts = window.PartitionKeys();
    for (size_t i = 0; i < parts.size(); ++i) {
        label += (i ? ", " : " partition=[") + parts[i];
    }
    if (!parts.empty()) {
        label += "]";
    }
    const auto& orders = window.OrderKeys();
    for (size_t i = 0; i < orders.size(); ++i) {
        label += (i ? ", " : " order=[") + SortKeyLabel(orders[i]);
    }
    if (!orders.empty()) {
        label += "]";
    }
    const auto& funcs = window.Functions();
    for (size_t i = 0; i < funcs.size(); ++i) {
        label += (i ? ", " : " fns=[") + funcs[i].Name + "=" + funcs[i].Func;
    }
    if (!funcs.empty()) {
        label += "]";
    }
    return label;
}

std::string PlanLabel(const TOperatorPtr& op) {
    if (auto source = TMaybeOp<TSourceOperator>(op)) {
        auto src = source.Cast();
        std::string label = "source " + src->SourcePath();
        if (!src->GetAlias().empty()) {
            label += " AS " + src->GetAlias();
        }
        if (src->RowGroupPredicate()) {
            label += " row-groups " + ExprLine(src->RowGroupPredicate());
        }
        return label;
    }
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        return "filter " + ExprLine(filter.Cast()->Predicate());
    }
    if (auto project = TMaybeOp<TProjectOperator>(op)) {
        std::string label = "project (";
        const auto& specs = project.Cast()->Projections();
        for (size_t i = 0; i < specs.size(); ++i) {
            label += (i ? ", " : "") + specs[i].Name;
        }
        return label + ")";
    }
    if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
        return AggregatePlanLabel(*aggregate.Cast());
    }
    if (auto join = TMaybeOp<TJoinOperator>(op)) {
        return JoinPlanLabel(*join.Cast());
    }
    if (auto sort = TMaybeOp<TSortOperator>(op)) {
        return SortPlanLabel("sort", sort.Cast()->Keys());
    }
    if (auto sort = TMaybeOp<TTopSortOperator>(op)) {
        return SortPlanLabel("top-sort", sort.Cast()->Keys(), sort.Cast()->Limit());
    }
    if (auto late = TMaybeOp<TLateMaterializeOperator>(op)) {
        return "late-materialize [" +
            std::to_string(late.Cast()->Columns().size()) + " columns]";
    }
    if (auto window = TMaybeOp<TWindowOperator>(op)) {
        return WindowPlanLabel(*window.Cast());
    }
    if (auto ref = TMaybeOp<TCteRef>(op)) {
        return "cte-ref #" + std::to_string(ref.Cast()->Def()->Id);
    }
    if (auto consumer = TMaybeOp<TCteConsumer>(op)) {
        return "cte-consumer #" + std::to_string(consumer.Cast()->Def()->Id)
            + " (×" + std::to_string(consumer.Cast()->Materialization()->RefCount) + ")";
    }
    return std::string(op->RelName());
}

void PrintPlanTree(
    std::ostream& out,
    const TOperatorPtr& op,
    const std::string& prefix,
    bool isLast,
    bool isRoot)
{
    out << prefix;
    if (!isRoot) {
        out << (isLast ? "└─ " : "├─ ");
    }
    out << PlanLabel(op);
    if (op->Stats_) {
        out << " (rows≈" << op->Stats_->RowCount
            << " cost≈" << FormatEstimate(op->Stats_->Cost) << ")";
    }
    out << "\n";

    auto children = op->Inputs();
    std::string childPrefix = isRoot ? prefix : prefix + (isLast ? "   " : "│  ");
    for (size_t i = 0; i < children.size(); ++i) {
        PrintPlanTree(out, children[i], childPrefix, i + 1 == children.size(), false);
    }
}

void PrintPlanTreeWithCtes(std::ostream& out, const TOperatorPtr& plan) {
    // Pre-reuse plans carry TCteRef definitions; post-reuse plans carry
    // TCteConsumer nodes over shared materializations. Print whichever applies.
    auto defs = CollectCteDefinitions(plan);
    for (const auto& def : defs) {
        out << "cte #" << def->Id << ":\n";
        PrintPlanTree(out, def->Plan);
    }
    auto mats = CollectMaterializations(plan);
    for (const auto& mat : mats) {
        out << "cte #" << mat.Id << " (materialized, ×"
            << mat.Materialization->RefCount << "):\n";
        PrintPlanTree(out, mat.Materialization->Plan);
    }
    if (!defs.empty() || !mats.empty()) {
        out << "main:\n";
    }
    PrintPlanTree(out, plan);
}

} // namespace NQdb
