#include <qdb/plan/ops/union.h>

namespace NQdb {

using namespace NQumir::NAst;

TUnionAllOperator::TUnionAllOperator(NQumir::NAst::TTypeFactory& types, std::vector<TOperatorPtr> inputs)
    : Inputs_(std::move(inputs))
{
    std::vector<TTypePtr> paramTypes;
    paramTypes.reserve(Inputs_.size());
    for (const auto& input : Inputs_) {
        paramTypes.push_back(input->OutputColumns());
    }
    auto output = Inputs_.empty() ? nullptr : Inputs_[0]->OutputColumns();
    Type = types.MakeFunction(std::move(paramTypes), std::move(output));
}

std::unordered_set<std::string> TUnionAllOperator::RequiredColumnsForChild(
    size_t childIdx, const std::unordered_set<std::string>& needed) const
{
    std::unordered_set<std::string> required;
    auto&& schema = Inputs_[childIdx]->OutputColumns();
    if (!schema) {
        return needed;
    }
    for (const auto& [name, _] : schema->Fields) {
        required.insert(name);
    }
    return required;
}

const std::string TUnionAllOperator::ToString() const {
    std::string s = "(rel union-all";
    for (const auto& input : Inputs_) {
        s += " " + input->ToString();
    }
    return s + ")";
}

} // namespace NQdb
