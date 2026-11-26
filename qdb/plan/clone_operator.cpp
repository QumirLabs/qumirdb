#include <qdb/plan/clone_operator.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/one_row.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/window.h>

#include <stdexcept>

namespace NQdb {

using namespace NQumir::NAst;

void CloneOperatorExprs(const TOperatorPtr& op) {
    if (!op) {
        return;
    }
    for (const auto& input : op->Inputs()) {
        CloneOperatorExprs(input);
    }
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        filter.Cast()->MutablePredicate() = CloneExpr(filter.Cast()->Predicate());
    } else if (auto project = TMaybeOp<TProjectOperator>(op)) {
        for (auto& spec : project.Cast()->MutableProjections()) {
            spec.Expression = CloneExpr(spec.Expression);
        }
    } else if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
        for (auto& spec : aggregate.Cast()->MutableAggs()) {
            if (spec.Arg) {
                spec.Arg = CloneExpr(spec.Arg);
            }
        }
    } else if (auto join = TMaybeOp<TJoinOperator>(op)) {
        if (join.Cast()->Filter()) {
            join.Cast()->MutableFilter() = CloneExpr(join.Cast()->Filter());
        }
    } else if (auto maybeWindow = TMaybeOp<TWindowOperator>(op)) {
        auto window = maybeWindow.Cast();
        for (auto& func : window->MutableFunctions()) {
            if (func.Arg) {
                func.Arg = CloneExpr(func.Arg);
            }
        }
        if (auto& frame = window->MutableFrame()) {
            if (frame->Start.Offset) {
                frame->Start.Offset = CloneExpr(frame->Start.Offset);
            }
            if (frame->End.Offset) {
                frame->End.Offset = CloneExpr(frame->End.Offset);
            }
        }
    }
}

namespace {

TOperatorPtr Reconstruct(NQumir::NAst::TTypeFactory& types, const TOperatorPtr& op);

// Carries over Type/Stats too: resolve runs after the passes, so lowering needs
// the annotated node, not a freshly-constructed one.
TOperatorPtr StructuralClone(NQumir::NAst::TTypeFactory& types, const TOperatorPtr& op) {
    if (!op) {
        return op;
    }
    auto clone = Reconstruct(types, op);
    clone->Type = op->Type;
    clone->Stats_ = op->Stats_;
    clone->AtMostOneRow = op->AtMostOneRow;
    clone->RuntimeFilterUses = op->RuntimeFilterUses;
    return clone;
}

TOperatorPtr Reconstruct(NQumir::NAst::TTypeFactory& types, const TOperatorPtr& op) {
    if (auto n = TMaybeOp<TSourceOperator>(op)) {
        auto src = NQumir::MakeIntrusive<TSourceOperator>(
            types, n.Cast()->GetTable(), n.Cast()->SourcePath());
        if (n.Cast()->EmitsRowId()) {
            src->EnableRowId(types);
        }
        if (!n.Cast()->GetAlias().empty()) {
            src->SetAlias(types, n.Cast()->GetAlias());
        }
        if (n.Cast()->RowGroupPredicate()) {
            src->SetRowGroupPredicate(CloneExpr(n.Cast()->RowGroupPredicate()));
        }
        return src;
    }
    if (TMaybeOp<TOneRowOperator>(op)) {
        return NQumir::MakeIntrusive<TOneRowOperator>(types);
    }
    if (auto n = TMaybeOp<TCteRef>(op)) {
        return NQumir::MakeIntrusive<TCteRef>(types, n.Cast()->Def());
    }
    if (auto n = TMaybeOp<TCteConsumer>(op)) {
        return NQumir::MakeIntrusive<TCteConsumer>(
            types, n.Cast()->Def(), n.Cast()->Materialization());
    }
    if (auto n = TMaybeOp<TFilterOperator>(op)) {
        return NQumir::MakeIntrusive<TFilterOperator>(
            types, StructuralClone(types, n.Cast()->Input()), n.Cast()->Predicate());
    }
    if (auto n = TMaybeOp<TProjectOperator>(op)) {
        return NQumir::MakeIntrusive<TProjectOperator>(
            types, StructuralClone(types, n.Cast()->Input()), n.Cast()->Projections());
    }
    if (auto n = TMaybeOp<TAggregateOperator>(op)) {
        auto agg = NQumir::MakeIntrusive<TAggregateOperator>(
            types, StructuralClone(types, n.Cast()->Input()), n.Cast()->GroupKeys(), n.Cast()->Aggs(),
            n.Cast()->IsGlobal());
        agg->MutableGroupingSets() = n.Cast()->GroupingSets();
        return agg;
    }
    if (auto n = TMaybeOp<TJoinOperator>(op)) {
        auto join = NQumir::MakeIntrusive<TJoinOperator>(
            types, StructuralClone(types, n.Cast()->Left()), StructuralClone(types, n.Cast()->Right()),
            n.Cast()->Keys(), n.Cast()->JoinType(), n.Cast()->Filter());
        // The constructor omits plan annotations.
        join->MutableDerivedSideFilters() = n.Cast()->DerivedSideFilters();
        join->MutableRuntimeFilter() = n.Cast()->RuntimeFilter();
        return join;
    }
    if (auto n = TMaybeOp<TUnionAllOperator>(op)) {
        std::vector<TOperatorPtr> inputs;
        inputs.reserve(n.Cast()->Inputs().size());
        for (const auto& input : n.Cast()->Inputs()) {
            inputs.push_back(StructuralClone(types, input));
        }
        return NQumir::MakeIntrusive<TUnionAllOperator>(types, std::move(inputs));
    }
    if (auto n = TMaybeOp<TSortOperator>(op)) {
        return NQumir::MakeIntrusive<TSortOperator>(
            types, StructuralClone(types, n.Cast()->Input()), n.Cast()->Keys());
    }
    if (auto n = TMaybeOp<TTopSortOperator>(op)) {
        return NQumir::MakeIntrusive<TTopSortOperator>(
            types, StructuralClone(types, n.Cast()->Input()), n.Cast()->Keys(), n.Cast()->Limit());
    }
    if (auto n = TMaybeOp<TLimitOperator>(op)) {
        return NQumir::MakeIntrusive<TLimitOperator>(
            types, StructuralClone(types, n.Cast()->Input()), n.Cast()->Limit(), n.Cast()->Offset());
    }
    if (auto n = TMaybeOp<TLateMaterializeOperator>(op)) {
        return NQumir::MakeIntrusive<TLateMaterializeOperator>(
            types, StructuralClone(types, n.Cast()->Input()),
            n.Cast()->LocatorColumn(),
            n.Cast()->Columns());
    }
    if (auto maybeWindow = TMaybeOp<TWindowOperator>(op)) {
        auto window = maybeWindow.Cast();
        auto clone = NQumir::MakeIntrusive<TWindowOperator>(
            types, StructuralClone(types, window->Input()), window->PartitionKeys(),
            window->OrderKeys(), window->Frame(), window->Functions());
        clone->MutablePartial() = window->Partial();
        return clone;
    }
    throw std::runtime_error(
        "CloneOperator: unsupported operator: " + std::string(op->RelName()));
}

} // namespace

TOperatorPtr CloneOperator(NQumir::NAst::TTypeFactory& types, const TOperatorPtr& op) {
    auto clone = StructuralClone(types, op);
    CloneOperatorExprs(clone);
    return clone;
}

} // namespace NQdb
