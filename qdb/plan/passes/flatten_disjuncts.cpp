#include "flatten_disjuncts.h"

namespace NQdb {

using namespace NQumir::NAst;

void FlattenDisjuncts(const TExprPtr& expr, std::vector<TExprPtr>& out) {
    auto binary = TMaybeNode<TBinaryExpr>(expr);
    if (binary && binary.Cast()->Operator == "||") {
        FlattenDisjuncts(binary.Cast()->Left(), out);
        FlattenDisjuncts(binary.Cast()->Right(), out);
    } else {
        out.push_back(expr);
    }
}

} // namespace NQdb
