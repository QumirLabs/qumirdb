#include <qdb/plan/passes/simplify_outer_joins.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

using TColumns = std::unordered_set<std::string>;

// Whether `expr` is NULL on every row whose `columns` are all NULL. Operators,
// casts and calls are strict as ExpandNullable evaluates them. AND and OR follow
// three-valued logic: FALSE AND NULL is FALSE, TRUE OR NULL is TRUE.
bool YieldsNull(const TExprPtr& expr, const TColumns& columns) {
    if (auto maybeIdent = TMaybeNode<TIdentExpr>(expr)) {
        return columns.contains(maybeIdent.Cast()->Name);
    }
    if (auto maybeBinary = TMaybeNode<TBinaryExpr>(expr)) {
        auto binary = maybeBinary.Cast();
        if (binary->Operator == "&&"_op || binary->Operator == "||"_op) {
            return YieldsNull(binary->Left, columns) && YieldsNull(binary->Right, columns);
        }
        return YieldsNull(binary->Left, columns) || YieldsNull(binary->Right, columns);
    }
    if (auto maybeUnary = TMaybeNode<TUnaryExpr>(expr)) {
        return YieldsNull(maybeUnary.Cast()->Operand, columns);
    }
    if (auto maybeCast = TMaybeNode<TCastExpr>(expr)) {
        return YieldsNull(maybeCast.Cast()->Operand, columns);
    }
    if (auto maybeCall = TMaybeNode<TCallExpr>(expr)) {
        auto call = maybeCall.Cast();
        auto maybeCallee = TMaybeNode<TIdentExpr>(call->Callee);
        if (!maybeCallee) {
            return false;
        }
        auto callee = maybeCallee.Cast();
        for (size_t index = 0; index < call->Args.size(); ++index) {
            if (NKernel::IsNullStrictCallArgument(callee->Name, index)
                && YieldsNull(call->Args[index], columns))
            {
                return true;
            }
        }
    }
    // CASE, literals and anything else are not known to be NULL.
    return false;
}

// The operand of IS NOT NULL, which reaches the planner as !qdb_is_null(x),
// or null for any other predicate.
TExprPtr IsNotNullOperand(const TExprPtr& predicate) {
    auto maybeUnary = TMaybeNode<TUnaryExpr>(predicate);
    if (!maybeUnary) {
        return nullptr;
    }
    auto unary = maybeUnary.Cast();
    if (unary->Operator != "!"_op) {
        return nullptr;
    }
    auto maybeCall = TMaybeNode<TCallExpr>(unary->Operand);
    if (!maybeCall) {
        return nullptr;
    }
    auto call = maybeCall.Cast();
    auto maybeCallee = TMaybeNode<TIdentExpr>(call->Callee);
    if (!maybeCallee || maybeCallee.Cast()->Name != "qdb_is_null" || call->Args.size() != 1) {
        return nullptr;
    }
    return call->Args.front();
}

} // namespace

bool RejectsNulls(const TExprPtr& predicate, const TColumns& columns) {
    if (auto maybeBinary = TMaybeNode<TBinaryExpr>(predicate)) {
        auto binary = maybeBinary.Cast();
        if (binary->Operator == "&&"_op) {
            return RejectsNulls(binary->Left, columns) || RejectsNulls(binary->Right, columns);
        }
        if (binary->Operator == "||"_op) {
            return RejectsNulls(binary->Left, columns) && RejectsNulls(binary->Right, columns);
        }
    }
    // IS NOT NULL is FALSE, not NULL, on a NULL operand.
    if (auto operand = IsNotNullOperand(predicate)) {
        return YieldsNull(operand, columns);
    }
    // A filter drops a row whose predicate is NULL as it drops FALSE.
    return YieldsNull(predicate, columns);
}

namespace {

// Predicates that every row a subtree contributes to the query result satisfies:
// a row on which one of them is not TRUE is dropped above the subtree, so the
// subtree need not produce it.
using TPredicates = std::vector<TExprPtr>;

bool AnyRejectsNulls(const TPredicates& predicates, const TOperatorPtr& input) {
    TColumns columns;
    for (const auto& [name, type] : input->OutputColumns()->Fields) {
        columns.insert(name);
    }
    return std::ranges::any_of(predicates, [&](const TExprPtr& predicate) {
        return RejectsNulls(predicate, columns);
    });
}

// Outer-join simplification (Galindo-Legaria and Rosenthal, "Outerjoin
// Simplification and Reordering for Query Optimization", ACM TODS 22(1), 1997;
// PostgreSQL's reduce_outer_joins). A predicate is null-rejecting for a set of
// columns when it cannot be TRUE on a row where all of them are NULL. The row an
// outer join adds for an unmatched row of one input has NULL in every column of
// the other input, so a predicate above the join that rejects those NULLs drops
// all such rows, and the narrower join that never produces them is equivalent:
// - A LEFT JOIN B becomes A JOIN B when the predicate rejects NULLs of B;
// - A RIGHT JOIN B becomes A JOIN B when it rejects NULLs of A;
// - A FULL JOIN B becomes A LEFT JOIN B when it rejects NULLs of A (the rows
//   added for B go), A RIGHT JOIN B when it rejects NULLs of B, and A JOIN B
//   when it rejects both.
EJoinType SimplifiedJoinType(const TJoinOperator& join, const TPredicates& above) {
    switch (join.JoinType()) {
        case EJoinType::Left:
            return AnyRejectsNulls(above, join.Right())
                ? EJoinType::Inner
                : EJoinType::Left;
        case EJoinType::Right:
            return AnyRejectsNulls(above, join.Left())
                ? EJoinType::Inner
                : EJoinType::Right;
        case EJoinType::Full: {
            const bool left = AnyRejectsNulls(above, join.Left());
            const bool right = AnyRejectsNulls(above, join.Right());
            if (left && right) {
                return EJoinType::Inner;
            }
            if (left) {
                return EJoinType::Left;
            }
            return right
                ? EJoinType::Right
                : EJoinType::Full;
        }
        default:
            return join.JoinType();
    }
}

TOperatorPtr Simplify(TOperatorPtr node, TPredicates above, bool& changed);

TOperatorPtr SimplifyJoin(
    const std::shared_ptr<TJoinOperator>& join,
    TPredicates above,
    bool& changed)
{
    const EJoinType type = SimplifiedJoinType(*join, above);

    TPredicates condition;
    if (join->Filter()) {
        condition.push_back(join->Filter());
    }
    for (const auto& key : join->Keys()) {
        if (!key.NullsEqual) {
            condition.push_back(std::make_shared<TBinaryExpr>(NQumir::TLocation{}, "=="_op,
                std::make_shared<TIdentExpr>(NQumir::TLocation{}, key.Left),
                std::make_shared<TIdentExpr>(NQumir::TLocation{}, key.Right)));
        }
    }

    // The predicates from above hold on each input that the output carries
    // without NULL extension. The join condition holds on each input whose
    // unmatched rows do not reach the output: it is TRUE on every row of that
    // input that matters. A FULL join passes nothing.
    TPredicates left;
    TPredicates right;
    switch (type) {
        case EJoinType::Inner:
            left = above;
            left.insert(left.end(), condition.begin(), condition.end());
            right = std::move(above);
            right.insert(right.end(), condition.begin(), condition.end());
            break;
        case EJoinType::Left:
        case EJoinType::LeftAnti:
            // For an anti join the condition goes to the subquery side. That is valid for
            // NOT EXISTS semantics only: once NOT IN is null-aware (#154), a NULL key on
            // the subquery side changes the result, so the condition must not go there.
            left = std::move(above);
            right = std::move(condition);
            break;
        case EJoinType::Right:
        case EJoinType::RightAnti:
            left = std::move(condition);
            right = std::move(above);
            break;
        case EJoinType::LeftSemi:
            left = std::move(above);
            left.insert(left.end(), condition.begin(), condition.end());
            right = std::move(condition);
            break;
        case EJoinType::RightSemi:
            right = std::move(above);
            right.insert(right.end(), condition.begin(), condition.end());
            left = std::move(condition);
            break;
        case EJoinType::Full:
        case EJoinType::LeftMark:
            break;
    }

    auto simplifiedLeft = Simplify(join->Left(), std::move(left), changed);
    auto simplifiedRight = Simplify(join->Right(), std::move(right), changed);
    if (type == join->JoinType()) {
        join->MutableLeft() = std::move(simplifiedLeft);
        join->MutableRight() = std::move(simplifiedRight);
        return join;
    }
    changed = true;
    auto simplified = std::make_shared<TJoinOperator>(
        std::move(simplifiedLeft), std::move(simplifiedRight), join->Keys(), type, join->Filter());
    simplified->Location = join->Location;
    return simplified;
}

TOperatorPtr Simplify(TOperatorPtr node, TPredicates above, bool& changed) {
    if (auto maybeFilter = TMaybeOp<TFilterOperator>(node)) {
        auto filter = maybeFilter.Cast();
        above.push_back(filter->Predicate());
        filter->MutableInput() = Simplify(filter->Input(), std::move(above), changed);
        return node;
    }
    if (auto maybeJoin = TMaybeOp<TJoinOperator>(node)) {
        return SimplifyJoin(maybeJoin.Cast(), std::move(above), changed);
    }
    // Other operators compute new columns or decide which rows reach the
    // predicates (a limit), so the predicates do not pass into their inputs.
    for (auto& input : node->MutableInputs()) {
        input = Simplify(input, {}, changed);
    }
    return node;
}

} // namespace

bool SimplifyOuterJoins(TOperatorPtr& root) {
    bool changed = false;
    root = Simplify(root, {}, changed);
    return changed;
}

} // namespace NQdb
