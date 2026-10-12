#include <qdb/plan/clone_operator.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
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

TOperatorPtr Reconstruct(const TOperatorPtr& op);

// Carries over Type/Stats too: resolve runs after the passes, so lowering needs
// the annotated node, not a freshly-constructed one.
TOperatorPtr StructuralClone(const TOperatorPtr& op) {
    if (!op) {
        return op;
    }
    auto clone = Reconstruct(op);
    clone->Type = op->Type;
    clone->Stats_ = op->Stats_;
    return clone;
}

TOperatorPtr Reconstruct(const TOperatorPtr& op) {
    if (auto n = TMaybeOp<TSourceOperator>(op)) {
        auto src = std::make_shared<TSourceOperator>(
            n.Cast()->GetSource(), n.Cast()->SourcePath());
        if (n.Cast()->EmitsRowId()) {
            src->EnableRowId();
        }
        if (!n.Cast()->GetAlias().empty()) {
            src->SetAlias(n.Cast()->GetAlias());
        }
        if (n.Cast()->RowGroupPredicate()) {
            src->SetRowGroupPredicate(CloneExpr(n.Cast()->RowGroupPredicate()));
        }
        return src;
    }
    if (auto n = TMaybeOp<TCteRef>(op)) {
        return std::make_shared<TCteRef>(n.Cast()->Def());
    }
    if (auto n = TMaybeOp<TCteConsumer>(op)) {
        return std::make_shared<TCteConsumer>(
            n.Cast()->Def(), n.Cast()->Materialization());
    }
    if (auto n = TMaybeOp<TFilterOperator>(op)) {
        return std::make_shared<TFilterOperator>(
            StructuralClone(n.Cast()->Input()), n.Cast()->Predicate());
    }
    if (auto n = TMaybeOp<TProjectOperator>(op)) {
        return std::make_shared<TProjectOperator>(
            StructuralClone(n.Cast()->Input()), n.Cast()->Projections());
    }
    if (auto n = TMaybeOp<TAggregateOperator>(op)) {
        auto agg = std::make_shared<TAggregateOperator>(
            StructuralClone(n.Cast()->Input()), n.Cast()->GroupKeys(), n.Cast()->Aggs());
        agg->MutableGroupingSets() = n.Cast()->GroupingSets();
        return agg;
    }
    if (auto n = TMaybeOp<TJoinOperator>(op)) {
        auto join = std::make_shared<TJoinOperator>(
            StructuralClone(n.Cast()->Left()), StructuralClone(n.Cast()->Right()),
            n.Cast()->Keys(), n.Cast()->JoinType(), n.Cast()->Filter());
        // The constructor omits plan annotations.
        join->MutableRuntimeFilter() = n.Cast()->RuntimeFilter();
        return join;
    }
    if (auto n = TMaybeOp<TUnionAllOperator>(op)) {
        std::vector<TOperatorPtr> inputs;
        inputs.reserve(n.Cast()->Inputs().size());
        for (const auto& input : n.Cast()->Inputs()) {
            inputs.push_back(StructuralClone(input));
        }
        return std::make_shared<TUnionAllOperator>(std::move(inputs));
    }
    if (auto n = TMaybeOp<TSortOperator>(op)) {
        return std::make_shared<TSortOperator>(
            StructuralClone(n.Cast()->Input()), n.Cast()->Keys());
    }
    if (auto n = TMaybeOp<TTopSortOperator>(op)) {
        return std::make_shared<TTopSortOperator>(
            StructuralClone(n.Cast()->Input()), n.Cast()->Keys(), n.Cast()->Limit());
    }
    if (auto n = TMaybeOp<TLimitOperator>(op)) {
        return std::make_shared<TLimitOperator>(
            StructuralClone(n.Cast()->Input()), n.Cast()->Limit(), n.Cast()->Offset());
    }
    if (auto n = TMaybeOp<TLateMaterializeOperator>(op)) {
        return std::make_shared<TLateMaterializeOperator>(
            StructuralClone(n.Cast()->Input()),
            n.Cast()->LocatorColumn(),
            n.Cast()->Columns());
    }
    if (auto n = TMaybeOp<TWindowOperator>(op)) {
        return std::make_shared<TWindowOperator>(
            StructuralClone(n.Cast()->Input()), n.Cast()->PartitionKeys(),
            n.Cast()->OrderKeys(), n.Cast()->Frame(), n.Cast()->Functions());
    }
    throw std::runtime_error(
        "CloneOperator: unsupported operator: " + std::string(op->RelName()));
}

} // namespace

TOperatorPtr CloneOperator(const TOperatorPtr& op) {
    auto clone = StructuralClone(op);
    CloneOperatorExprs(clone);
    return clone;
}

} // namespace NQdb
