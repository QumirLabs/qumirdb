#include <qdb/plan/passes/top_sort.h>

#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/sort.h>

#include <limits>

namespace NQdb {

TOperatorPtr ApplyTopSort(NQumir::NAst::TTypeFactory& types, const TOperatorPtr& root) {
    if (!root) {
        return root;
    }

    if (auto maybeLimit = TMaybeOp<TLimitOperator>(root)) {
        auto limit = maybeLimit.Cast();
        limit->MutableInput() = ApplyTopSort(types, limit->Input());
        auto maybeSort = TMaybeOp<TSortOperator>(limit->Input());
        if (!maybeSort) {
            return root;
        }
        auto sort = maybeSort.Cast();
        const int64_t rows = limit->Limit();
        const int64_t offset = limit->Offset();
        if (offset == 0) {
            return NQumir::MakeIntrusive<TTopSortOperator>(
                types, sort->Input(), sort->Keys(), rows);
        }
        // The rows an offset skips are the first rows of the order, so a
        // top-sort of limit + offset rows keeps everything the limit returns.
        if (rows <= std::numeric_limits<int64_t>::max() - offset) {
            limit->MutableInput() = NQumir::MakeIntrusive<TTopSortOperator>(
                types, sort->Input(), sort->Keys(), rows + offset);
        }
        return root;
    }

    for (auto& input : root->MutableInputs()) {
        input = ApplyTopSort(types, input);
    }

    return root;
}

} // namespace NQdb
