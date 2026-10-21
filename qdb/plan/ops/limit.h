#pragma once

#include <qdb/plan/ops/operator.h>

#include <cstdint>

namespace NQdb {

class TLimitOperator : public IOperator {
public:
    static constexpr const char* OpId = "limit";

    TLimitOperator(TOperatorPtr input, int64_t limit, int64_t offset = 0);

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override { return {}; }
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }
    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    TOperatorPtr& MutableInput() { return Input_; }
    int64_t Limit() const { return Limit_; }
    int64_t Offset() const { return Offset_; }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(&Input_, 1);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(&Input_, 1);
    }

private:
    TOperatorPtr Input_;
    int64_t Limit_ = 0;
    int64_t Offset_ = 0;
};

} // namespace NQdb
