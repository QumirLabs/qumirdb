#include <qdb/plan/ops/one_row.h>

#include <qumir/parser/type.h>

#include <memory>

namespace NQdb {

using namespace NQumir::NAst;

TOneRowOperator::TOneRowOperator(NQumir::NAst::TTypeFactory& types)
{
    Type = types.MakeFunction(
        std::vector<TTypePtr>{},
        types.MakeStruct(std::vector<std::pair<std::string, TTypePtr>>{}));
}

const std::string TOneRowOperator::ToString() const
{
    return "(rel one-row)";
}

} // namespace NQdb
