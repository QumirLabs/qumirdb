#include <qdb/plan/ops/late_materialize.h>

#include <qdb/io/io.h>
#include <qdb/plan/ops/source.h>

#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace NQdb {
namespace {

void CollectInputSources(
    const TOperatorPtr& root,
    std::vector<TSourceOperator*>& sources)
{
    if (auto source = TMaybeOp<TSourceOperator>(root)) {
        sources.push_back(source.Cast().get());
        return;
    }
    for (const auto& input : root->Inputs()) {
        CollectInputSources(input, sources);
    }
}

} // namespace

TLateMaterializeOperator::TLateMaterializeOperator(
    TOperatorPtr input,
    std::string locatorColumn,
    std::vector<TLateMaterializeColumn> columns)
    : Input_(std::move(input))
    , LocatorColumn_(std::move(locatorColumn))
    , Columns_(std::move(columns))
{
    using namespace NQumir::NAst;
    std::vector<std::pair<std::string, TTypePtr>> fields;
    fields.reserve(Columns_.size());
    for (const auto& column : Columns_) {
        fields.emplace_back(column.OutputName, column.Type);
    }
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{Input_->OutputColumns()},
        std::make_shared<TStructType>(std::move(fields)));
}

std::vector<TColumnReference> TLateMaterializeOperator::ColumnReferences() const {
    return {{.Name = LocatorColumn_}};
}

std::unordered_set<std::string> TLateMaterializeOperator::RequiredColumnsForChild(
    size_t,
    const std::unordered_set<std::string>&) const
{
    return {LocatorColumn_};
}

const std::string TLateMaterializeOperator::ToString() const {
    using NQumir::NAst::NCore::PrintType;

    std::string result = "(rel late-materialize " + Input_->ToString() +
        " (locator " + LocatorColumn_ + ")";
    for (const auto& column : Columns_) {
        result += " (column " + column.OutputName + " " +
            column.PhysicalName + " " + PrintType(column.Type) + ")";
    }
    return result + ")";
}

TLateMaterializationSourceBinding ResolveLateMaterializationSource(
    const TLateMaterializeOperator& late)
{
    std::vector<TSourceOperator*> sources;
    CollectInputSources(late.Input(), sources);
    if (sources.size() != 1) {
        throw std::runtime_error(
            "late materialize requires exactly one input source");
    }
    auto* lookup = dynamic_cast<IRowLookupSource*>(
        &sources.front()->GetSource());
    if (!lookup) {
        throw std::runtime_error(
            "late materialize source does not support physical row lookup");
    }
    return {
        .Source = *sources.front(),
        .Lookup = *lookup,
    };
}

} // namespace NQdb
