#include <qdb/plan/ops/limit.h>

#include <qumir/parser/type.h>

#include <memory>
#include <utility>

namespace NQdb {

using namespace NQumir::NAst;

TLimitOperator::TLimitOperator(NQumir::NAst::TTypeFactory& types, TOperatorPtr input, int64_t limit, int64_t offset)
    : Input_(std::move(input))
    , Limit_(limit)
    , Offset_(offset)
{
    auto inputSchema = Input_->OutputColumns();
    Type = types.MakeFunction(
        std::vector<TTypePtr>{inputSchema},
        inputSchema);
}

const std::string TLimitOperator::ToString() const {
    return "(rel limit " + Input_->ToString() + " " +
        std::to_string(Limit_) + " " + std::to_string(Offset_) + ")";
}

} // namespace NQdb
