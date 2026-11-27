#include <qdb/plan/ops/project.h>

#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/type_text.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>

namespace NQdb {

size_t FlattenedArity(const NQumir::NAst::TTypePtr& type) {
    const auto* record = RecordFields(type);
    return record ? record->Fields.size() : 1;
}

std::vector<TProjectionSpec> PassThroughProjections(const NQumir::NAst::TStructType& schema) {
    std::vector<TProjectionSpec> projections;
    projections.reserve(schema.Fields.size());
    for (const auto& [name, _] : schema.Fields) {
        projections.push_back({
            .Name = name,
            .Expression = NQumir::MakeIntrusive<NQumir::NAst::TIdentExpr>(NQumir::TLocation{}, name),
        });
    }
    return projections;
}

size_t FlattenedProjectionArity(const TProjectionSpec& projection) {
    return projection.Expression
        ? FlattenedArity(projection.Expression->Type)
        : 1;
}

TProjectOperator::TProjectOperator(NQumir::NAst::TTypeFactory& types, TOperatorPtr input, std::vector<TProjectionSpec> projections)
    : Input_(std::move(input))
    , Projections_(std::move(projections))
{
    auto&& inputStruct = Input_->OutputColumns();
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outFields;
    if (inputStruct) {
        for (const auto& proj : Projections_) {
            NQumir::NAst::TTypePtr fieldType = nullptr;
            if (auto ident = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(proj.Expression)) {
                for (const auto& [name, type] : inputStruct->Fields) {
                    if (name == ident.Cast()->Name) { fieldType = type; break; }
                }
            }
            if (proj.ColumnAliases.empty()) {
                outFields.emplace_back(proj.Name, fieldType);
            } else {
                for (const auto& name : proj.ColumnAliases) {
                    outFields.emplace_back(name, nullptr);
                }
            }
        }
    }
    // ParamTypes[0] = full input schema initially; narrowed by column push-down.
    Type = types.MakeFunction(
        std::vector<NQumir::NAst::TTypePtr>{Input_->OutputColumns()},
        types.MakeStruct(std::move(outFields)));
}

std::vector<TColumnReference> TProjectOperator::ColumnReferences() const {
    std::vector<TColumnReference> references;
    for (const auto& proj : Projections_) {
        AppendColumnReferences(references, proj.Expression);
    }
    return references;
}

const std::string TProjectOperator::ToString() const {
    std::string s = "(rel project " + Input_->ToString();
    for (const auto& p : Projections_) {
        s += " (" + p.Name + " " + NQumir::NAst::NCore::PrintAst(p.Expression) + ")";
    }
    return s + ")";
}

std::expected<TOperatorPtr, NQumir::TError>
MakeProject(NQumir::NAst::TTypeFactory& types, TOperatorPtr input, std::vector<std::pair<std::string, std::string>> projections) {
    std::vector<TProjectionSpec> specs;
    specs.reserve(projections.size());
    for (auto& [name, exprStr] : projections) {
        std::istringstream ss(exprStr);
        NQumir::NAst::NCore::TTokenStream tokens(ss);
        NQumir::NAst::NCore::TParser parser(types);
        auto parsed = parser.Parse(tokens);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        specs.push_back({std::move(name), std::move(*parsed)});
    }
    return NQumir::MakeIntrusive<TProjectOperator>(types, std::move(input), std::move(specs));
}

} // namespace NQdb
