#pragma once

#include <qdb/plan/ops/operator.h>

#include <string>
#include <utility>
#include <vector>

namespace NQdb {

// UNION ALL: concatenates the rows of N branches, which must agree on column
// count and types. Output schema is the first branch's (its column names).
// Logical-only - the scheduler lowers it away into a plain fan-in (no task).
class TUnionAllOperator : public IOperator {
public:
    static constexpr const char* OpId = "union-all";

    explicit TUnionAllOperator(std::vector<TOperatorPtr> inputs);

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override { return {}; }
    // Each branch defines its own schema, so parent-required names (in the first
    // branch's namespace) do not translate; keep every column the branch produces.
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t childIdx, const std::unordered_set<std::string>& needed) const override;

    std::vector<NQumir::NAst::TExprPtr> Children() const override {
        return std::vector<NQumir::NAst::TExprPtr>(Inputs_.begin(), Inputs_.end());
    }
    const std::string ToString() const override;

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(Inputs_);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(Inputs_);
    }

    void SetInputs(std::vector<TOperatorPtr> inputs) {
        Inputs_ = std::move(inputs);
    }

private:
    std::vector<TOperatorPtr> Inputs_;
};

} // namespace NQdb
