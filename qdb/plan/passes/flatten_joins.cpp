#include <qdb/plan/passes/flatten_joins.h>

#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>

#include <algorithm>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

TExprPtr Eq(const std::string& l, const std::string& r) {
    return std::make_shared<TBinaryExpr>(NQumir::TLocation{}, TOperator("=="),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, l),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, r));
}

TExprPtr Conjoin(const std::vector<TExprPtr>& parts) {
    TExprPtr result;
    for (const auto& p : parts) {
        result = result
            ? std::make_shared<TBinaryExpr>(p->Location, TOperator("&&"), result, p)
            : p;
    }
    return result;
}

// Flattening would discard the annotation with its join node.
bool IsInner(const TOperatorPtr& node) {
    auto join = TMaybeOp<TJoinOperator>(node);
    // Flattening represents keys as ordinary equality predicates. Keep joins
    // with NULL-equal keys or runtime filters intact so that conversion
    // preserves their semantics and annotations.
    return join && join.Cast()->JoinType() == EJoinType::Inner
        && !join.Cast()->RuntimeFilter()
        && std::ranges::none_of(join.Cast()->Keys(), [](const TJoinKey& key) {
            return key.NullsEqual;
        });
}

void Collect(const TOperatorPtr& node, std::vector<TOperatorPtr>& leaves, std::vector<TExprPtr>& conds) {
    if (!IsInner(node)) {
        leaves.push_back(node);
        return;
    }
    auto join = TMaybeOp<TJoinOperator>(node).Cast();
    if (join->Filter()) {
        conds.push_back(join->Filter());
    }
    for (const auto& key : join->Keys()) {
        conds.push_back(Eq(key.Left, key.Right));
    }
    Collect(join->Left(), leaves, conds);
    Collect(join->Right(), leaves, conds);
}

} // namespace

TOperatorPtr FlattenInnerJoins(TOperatorPtr root) {
    if (!root) {
        return root;
    }
    if (IsInner(root)) {
        std::vector<TOperatorPtr> leaves;
        std::vector<TExprPtr> conds;
        Collect(root, leaves, conds);
        TOperatorPtr chain;
        for (const auto& leaf : leaves) {
            auto flat = FlattenInnerJoins(leaf);
            chain = chain
                ? std::make_shared<TJoinOperator>(
                      chain, flat, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr)
                : flat;
        }
        return conds.empty()
            ? chain
            : std::make_shared<TFilterOperator>(chain, Conjoin(conds));
    }
    for (auto& input : root->MutableInputs()) {
        input = FlattenInnerJoins(input);
    }
    return root;
}

} // namespace NQdb
