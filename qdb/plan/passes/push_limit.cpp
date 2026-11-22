#include <qdb/plan/passes/push_limit.h>

#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>

namespace NQdb {

TOperatorPtr PushDownLimits(NQumir::NAst::TTypeFactory& types, const TOperatorPtr& root) {
    if (!root) {
        return root;
    }

    if (auto limit = TMaybeOp<TLimitOperator>(root)) {
        auto input = PushDownLimits(types, limit.Cast()->Input());
        if (auto project = TMaybeOp<TProjectOperator>(input)) {
            auto pushed = NQumir::MakeIntrusive<TLimitOperator>(
                types, project.Cast()->Input(),
                limit.Cast()->Limit(),
                limit.Cast()->Offset());
            project.Cast()->MutableInput() = PushDownLimits(types, pushed);
            return project.Cast();
        }
        limit.Cast()->MutableInput() = input;
        return root;
    }

    for (auto& input : root->MutableInputs()) {
        input = PushDownLimits(types, input);
    }

    return root;
}

} // namespace NQdb
