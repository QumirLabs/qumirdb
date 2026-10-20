#pragma once

#include <qdb/plan/ops/operator.h>

#include <string>
#include <vector>

namespace NQdb {

enum class ESortDirection {
    Asc,
    Desc,
};

enum class ESortNulls {
    Default,
    First,
    Last,
};

struct TSortKey {
    std::string Column;
    ESortDirection Direction = ESortDirection::Asc;
    ESortNulls Nulls = ESortNulls::Default;
};

class TSortOperator : public IOperator {
public:
    static constexpr const char* OpId = "sort";

    TSortOperator(TOperatorPtr input, std::vector<TSortKey> keys);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }
    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    TOperatorPtr& MutableInput() { return Input_; }
    const std::vector<TSortKey>& Keys() const { return Keys_; }
    std::vector<TSortKey>& MutableKeys() { return Keys_; }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(&Input_, 1);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(&Input_, 1);
    }

private:
    TOperatorPtr Input_;
    std::vector<TSortKey> Keys_;
};

class TTopSortOperator : public IOperator {
public:
    static constexpr const char* OpId = "top-sort";

    TTopSortOperator(TOperatorPtr input, std::vector<TSortKey> keys, int64_t limit);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }
    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    TOperatorPtr& MutableInput() { return Input_; }
    const std::vector<TSortKey>& Keys() const { return Keys_; }
    std::vector<TSortKey>& MutableKeys() { return Keys_; }
    int64_t Limit() const { return Limit_; }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(&Input_, 1);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(&Input_, 1);
    }

private:
    TOperatorPtr Input_;
    std::vector<TSortKey> Keys_;
    int64_t Limit_ = 0;
};

std::string_view SortDirectionName(ESortDirection direction);
std::string_view SortNullsName(ESortNulls nulls);

} // namespace NQdb
