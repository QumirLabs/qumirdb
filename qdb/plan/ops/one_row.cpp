#include <qdb/plan/ops/one_row.h>

#include <qumir/parser/type.h>

#include <memory>

namespace NQdb {

using namespace NQumir::NAst;

TOneRowOperator::TOneRowOperator()
{
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{},
        std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{}));
}

const std::string TOneRowOperator::ToString() const
{
    return "(rel one-row)";
}

} // namespace NQdb
