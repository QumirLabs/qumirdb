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

namespace {

using TColumns = std::unordered_set<std::string>;

// Whether `expr` is NULL on every row whose `columns` are all NULL. Operators,
// casts and calls are strict as ExpandNullable evaluates them. AND and OR follow
// three-valued logic: FALSE AND NULL is FALSE, TRUE OR NULL is TRUE.
bool YieldsNull(const TExprPtr& expr, const TColumns& columns) {
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        return columns.contains(ident.Cast()->Name);
    }
    if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
        const auto& node = binary.Cast();
        if (node->Operator == "&&" || node->Operator == "||") {
            return YieldsNull(node->Left, columns) && YieldsNull(node->Right, columns);
        }
        return YieldsNull(node->Left, columns) || YieldsNull(node->Right, columns);
    }
    if (auto unary = TMaybeNode<TUnaryExpr>(expr)) {
        return YieldsNull(unary.Cast()->Operand, columns);
    }
    if (auto cast = TMaybeNode<TCastExpr>(expr)) {
        return YieldsNull(cast.Cast()->Operand, columns);
    }
    if (auto call = TMaybeNode<TCallExpr>(expr)) {
        auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee);
        const auto& args = call.Cast()->Args;
        for (size_t index = 0; callee && index < args.size(); ++index) {
            if (NKernel::IsNullStrictCallArgument(callee.Cast()->Name, index)
                && YieldsNull(args[index], columns))
            {
                return true;
            }
        }
    }
    // CASE, literals and anything else are not known to be NULL.
    return false;
}

} // namespace

bool RejectsNulls(const TExprPtr& predicate, const TColumns& columns) {
    if (auto binary = TMaybeNode<TBinaryExpr>(predicate)) {
        const auto& node = binary.Cast();
        if (node->Operator == "&&") {
            return RejectsNulls(node->Left, columns) || RejectsNulls(node->Right, columns);
        }
        if (node->Operator == "||") {
            return RejectsNulls(node->Left, columns) && RejectsNulls(node->Right, columns);
        }
    }
    // IS NOT NULL is FALSE, not NULL, on a NULL operand.
    if (auto unary = TMaybeNode<TUnaryExpr>(predicate); unary && unary.Cast()->Operator == "!") {
        if (auto call = TMaybeNode<TCallExpr>(unary.Cast()->Operand)) {
            auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee);
            if (callee && callee.Cast()->Name == "qdb_is_null" && call.Cast()->Args.size() == 1) {
                return YieldsNull(call.Cast()->Args.front(), columns);
            }
        }
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
            condition.push_back(std::make_shared<TBinaryExpr>(NQumir::TLocation{}, TOperator("=="),
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
    if (auto filter = TMaybeOp<TFilterOperator>(node)) {
        above.push_back(filter.Cast()->Predicate());
        filter.Cast()->MutableInput() = Simplify(filter.Cast()->Input(), std::move(above), changed);
        return node;
    }
    if (auto join = TMaybeOp<TJoinOperator>(node)) {
        return SimplifyJoin(join.Cast(), std::move(above), changed);
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
