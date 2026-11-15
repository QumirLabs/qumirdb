#include "junction.h"

#include <qdb/utils/balanced_fold.h>

namespace NQdb {

using namespace NQumir::NAst;

TExprPtr Junction(TOperator op, std::vector<TExprPtr> parts) {
    return FoldBalanced(std::move(parts), [op](TExprPtr left, TExprPtr right) -> TExprPtr {
        auto location = left->Location;
        return std::make_shared<TBinaryExpr>(std::move(location), op, std::move(left), std::move(right));
    });
}

} // namespace NQdb
