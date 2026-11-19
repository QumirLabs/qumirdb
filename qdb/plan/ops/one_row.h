#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// The relation of one row and no columns: the input of a SELECT without FROM.
class TOneRowOperator : public IOperator {
public:
    static constexpr const char* OpId = "one-row";

    TOneRowOperator();

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override { return {}; }
    const std::string ToString() const override;
};

} // namespace NQdb
