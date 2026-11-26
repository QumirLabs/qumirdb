#include <qdb/plan/passes/window_top_k.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/passes/flatten_conjuncts.h>

#include <algorithm>
#include <string>

namespace NQdb {

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

// `conjunct` with `rank` renamed to `local` when it is `rank <= K`, `rank < K`,
// `K >= rank` or `K > rank` for a constant K; otherwise null.
TExprPtr LocalRankBound(
    const TExprPtr& conjunct,
    const std::string& rank,
    const std::string& local,
    TConstFolder& folder)
{
    auto maybeBinary = TMaybeNode<TBinaryExpr>(conjunct);
    if (!maybeBinary) {
        return nullptr;
    }
    auto binary = maybeBinary.Cast();
    const bool upper = binary->Operator == "<="_op || binary->Operator == "<"_op;
    const bool lower = binary->Operator == ">="_op || binary->Operator == ">"_op;
    if (!upper && !lower) {
        return nullptr;
    }
    auto maybeColumn = TMaybeNode<TIdentExpr>(upper ? binary->Left() : binary->Right());
    const auto& bound = upper ? binary->Right() : binary->Left();
    if (!maybeColumn || maybeColumn.Cast()->Name != rank || !folder.FoldNumber(bound)) {
        return nullptr;
    }
    TExprPtr column = NQumir::MakeIntrusive<TIdentExpr>(binary->Location, local);
    auto value = CloneExpr(bound);
    return NQumir::MakeIntrusive<TBinaryExpr>(
        binary->Location, binary->Operator,
        upper ? column : value,
        upper ? value : column);
}

// A plan optimized before (a sexpr query pasted from EXPLAIN) already has one.
bool HasPartialTopK(const TWindowOperator& window)
{
    auto maybeFilter = TMaybeOp<TFilterOperator>(window.Input());
    if (!maybeFilter) {
        return false;
    }
    auto filter = maybeFilter.Cast();
    auto maybePartial = TMaybeOp<TWindowOperator>(filter->Input());
    return maybePartial && maybePartial.Cast()->Partial();
}

void Rewrite(
    TTypeFactory& types,
    TFilterOperator& filter,
    TWindowOperator& window,
    TConstFolder& folder,
    uint32_t& nextId)
{
    // Any other function (sum, max, ...) reads rows the partial filter drops.
    const auto& functions = window.Functions();
    if (window.Partial() || HasPartialTopK(window)
        || !std::ranges::all_of(functions, [](const TWindowFunc& func) {
            return IsRankingWindowFunction(func.Func);
        }))
    {
        return;
    }
    std::vector<TExprPtr> conjuncts;
    FlattenConjuncts(filter.Predicate(), conjuncts);
    const std::string local = "__topk_" + std::to_string(nextId) + "__";
    for (const auto& conjunct : conjuncts) {
        for (const auto& func : functions) {
            // Without ORDER BY every row is a peer: rank and dense_rank are all 1.
            if (window.OrderKeys().empty() && func.Func != "row_number") {
                continue;
            }
            auto bound = LocalRankBound(conjunct, func.Name, local, folder);
            if (!bound) {
                continue;
            }
            // Ranking functions ignore the frame.
            auto partial = NQumir::MakeIntrusive<TWindowOperator>(
                types, window.Input(), window.PartitionKeys(), window.OrderKeys(),
                std::nullopt, std::vector<TWindowFunc>{{.Name = local, .Func = func.Func}});
            partial->MutablePartial() = true;
            window.MutableInput() = NQumir::MakeIntrusive<TFilterOperator>(
                types, std::move(partial), std::move(bound));
            ++nextId;
            return;
        }
    }
}

} // namespace

void ApplyWindowTopK(
    TTypeFactory& types,
    const TOperatorPtr& root,
    TConstFolder& folder,
    uint32_t& nextId)
{
    if (!root) {
        return;
    }
    for (const auto& input : root->Inputs()) {
        ApplyWindowTopK(types, input, folder, nextId);
    }
    auto maybeFilter = TMaybeOp<TFilterOperator>(root);
    if (!maybeFilter) {
        return;
    }
    auto filter = maybeFilter.Cast();
    auto maybeWindow = TMaybeOp<TWindowOperator>(filter->Input());
    if (!maybeWindow) {
        return;
    }
    auto window = maybeWindow.Cast();
    Rewrite(types, *filter, *window, folder, nextId);
}

} // namespace NQdb
