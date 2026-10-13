#include <qdb/plan/passes/self_equality.h>

#include <qdb/plan/ops/filter.h>
#include <qdb/plan/types/nullable.h>

namespace NQdb {

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

// Returns nullptr when the predicate is always TRUE.
TExprPtr Simplify(const TExprPtr& expr, const TStructType& schema) {
    auto maybeBinary = TMaybeNode<TBinaryExpr>(expr);
    if (!maybeBinary) {
        return expr;
    }
    auto binary = maybeBinary.Cast();
    if (binary->Operator == "&&"_op || binary->Operator == "||"_op) {
        auto left = Simplify(binary->Left, schema);
        auto right = Simplify(binary->Right, schema);
        if (binary->Operator == "||"_op) {
            if (!left || !right) {
                return nullptr;
            }
        } else if (!left || !right) {
            return left ? left : right;
        }
        if (left == binary->Left && right == binary->Right) {
            return expr;
        }
        return std::make_shared<TBinaryExpr>(
            binary->Location, binary->Operator, std::move(left), std::move(right));
    }
    if (binary->Operator != "=="_op) {
        return expr;
    }
    auto maybeLeft = TMaybeNode<TIdentExpr>(binary->Left);
    auto maybeRight = TMaybeNode<TIdentExpr>(binary->Right);
    if (!maybeLeft || !maybeRight) {
        return expr;
    }
    auto left = maybeLeft.Cast();
    if (left->Name != maybeRight.Cast()->Name) {
        return expr;
    }
    for (const auto& [name, type] : schema.Fields) {
        if (name != left->Name) {
            continue;
        }
        // NaN != NaN, so a float column does not equal itself on every row.
        if (TMaybeType<TFloatType>(UnwrapNamedType(UnwrapNullableType(type)))) {
            return expr;
        }
        if (!IsNullableType(type)) {
            return nullptr;
        }
        // NULL = NULL is unknown, which the filter drops like FALSE.
        return std::make_shared<TUnaryExpr>(
            binary->Location, "!"_op,
            std::make_shared<TCallExpr>(
                binary->Location,
                std::make_shared<TIdentExpr>(binary->Location, "qdb_is_null"),
                std::vector<TExprPtr>{binary->Left}));
    }
    return expr;
}

} // namespace

TOperatorPtr SimplifySelfEquality(TOperatorPtr root) {
    if (!root) {
        return root;
    }
    for (auto& input : root->MutableInputs()) {
        input = SimplifySelfEquality(input);
    }
    auto maybeFilter = TMaybeOp<TFilterOperator>(root);
    if (!maybeFilter) {
        return root;
    }
    auto filter = maybeFilter.Cast();
    auto schema = filter->Input()->OutputColumns();
    if (!schema) {
        return root;
    }
    auto predicate = Simplify(filter->Predicate(), *schema);
    if (!predicate) {
        return filter->Input();
    }
    filter->MutablePredicate() = std::move(predicate);
    return root;
}

} // namespace NQdb
