#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/type.h>

#include <functional>

namespace NQdb {

namespace {

NQumir::NAst::TTypePtr SourceOutputType(
    const ITable& table,
    const std::string& alias,
    bool emitRowId)
{
    using namespace NQumir::NAst;
    std::vector<std::pair<std::string, TTypePtr>> fields;
    auto& columns = table.Schema().Columns;
    fields.reserve(columns.size() + (emitRowId ? 1 : 0));
    for (const auto& col : columns) {
        auto name = std::string(col.Name);
        fields.emplace_back(alias.empty() ? name : alias + "." + name, col.Type);
    }
    if (emitRowId) {
        const std::string rowIdName(InternalRowIdColumnName);
        fields.emplace_back(
            alias.empty() ? rowIdName : alias + "." + rowIdName,
            std::make_shared<TIntegerType>(TIntegerType::U64));
    }
    return std::make_shared<TStructType>(std::move(fields));
}

} // namespace

NQumir::NAst::TTypePtr StructTypeFromSchema(const TSchema& schema) {
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
    fields.reserve(schema.Columns.size());
    for (const auto& col : schema.Columns) {
        fields.emplace_back(std::string(col.Name), col.Type);
    }
    return std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
}

TSourceOperator::TSourceOperator(std::shared_ptr<const ITable> table, std::string path)
    : Table_(std::move(table))
    , SourcePath_(std::move(path))
{
    Type = std::make_shared<NQumir::NAst::TFunctionType>(
        std::vector<NQumir::NAst::TTypePtr>{},
        StructTypeFromSchema(Table_->Schema()));
}

void TSourceOperator::SetAlias(std::string alias) {
    Alias_ = std::move(alias);
    if (auto* fun = static_cast<NQumir::NAst::TFunctionType*>(Type.get())) {
        fun->ReturnType = SourceOutputType(*Table_, Alias_, EmitRowId_);
    }
}

void TSourceOperator::EnableRowId() {
    EmitRowId_ = true;
    if (auto* fun = static_cast<NQumir::NAst::TFunctionType*>(Type.get())) {
        fun->ReturnType = SourceOutputType(*Table_, Alias_, true);
    }
}

std::string TSourceOperator::RowIdColumn() const {
    const std::string rowIdName(InternalRowIdColumnName);
    return Alias_.empty() ? rowIdName : Alias_ + "." + rowIdName;
}

void TSourceOperator::RestrictColumns(const std::unordered_set<std::string>& names) {
    RestrictedColumns_ = std::vector<std::string>(names.begin(), names.end());
    std::sort(RestrictedColumns_->begin(), RestrictedColumns_->end());
}

std::optional<std::vector<std::string>> TSourceOperator::RestrictedColumns() const {
    return RestrictedColumns_;
}

const std::string TSourceOperator::ToString() const {
    return "(rel source)";
}

} // namespace NQdb
