#include "flatten_conjuncts.h"

namespace NQdb {

using namespace NQumir::NAst;

void FlattenConjuncts(const TExprPtr& expr, std::vector<TExprPtr>& out) {
    auto binary = TMaybeNode<TBinaryExpr>(expr);
    if (binary && binary.Cast()->Operator == "&&") {
        FlattenConjuncts(binary.Cast()->Left(), out);
        FlattenConjuncts(binary.Cast()->Right(), out);
    } else {
        out.push_back(expr);
    }
}


} // namespace
