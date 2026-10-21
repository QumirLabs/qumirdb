#pragma once

#include <qdb/plan/ops/operator.h>

#include <qumir/error.h>

#include <expected>
#include <string>

namespace NQdb {

class TFilterOperator : public IOperator {
public:
    static constexpr const char* OpId = "filter";

    TFilterOperator(TOperatorPtr input, NQumir::NAst::TExprPtr predicate);

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override;
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }
    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    TOperatorPtr& MutableInput() { return Input_; }
    const NQumir::NAst::TExprPtr& Predicate() const { return Predicate_; }
    NQumir::NAst::TExprPtr& MutablePredicate() { return Predicate_; }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(&Input_, 1);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(&Input_, 1);
    }

private:
    TOperatorPtr Input_;
    NQumir::NAst::TExprPtr Predicate_; // parsed, unannotated
};

std::expected<TOperatorPtr, NQumir::TError>
MakeFilter(TOperatorPtr input, const std::string& predicate);

} // namespace NQdb
