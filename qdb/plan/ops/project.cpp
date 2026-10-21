#include <qdb/plan/ops/project.h>

#include <qdb/plan/types/nullable.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>

namespace NQdb {

size_t FlattenedProjectionArity(const TProjectionSpec& projection) {
    using namespace NQumir::NAst;
    if (!projection.Expression || IsNullableType(projection.Expression->Type)) {
        return 1;
    }
    if (auto structure = TMaybeType<TStructType>(UnwrapNamedType(projection.Expression->Type)))
    {
        return structure.Cast()->Fields.size();
    }
    return 1;
}

TProjectOperator::TProjectOperator(TOperatorPtr input, std::vector<TProjectionSpec> projections)
    : Input_(std::move(input))
    , Projections_(std::move(projections))
{
    auto&& inputStruct = Input_->OutputColumns();
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outFields;
    if (inputStruct) {
        for (const auto& proj : Projections_) {
            NQumir::NAst::TTypePtr fieldType;
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
    Type = std::make_shared<NQumir::NAst::TFunctionType>(
        std::vector<NQumir::NAst::TTypePtr>{Input_->OutputColumns()},
        std::make_shared<NQumir::NAst::TStructType>(std::move(outFields)));
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
MakeProject(TOperatorPtr input, std::vector<std::pair<std::string, std::string>> projections) {
    std::vector<TProjectionSpec> specs;
    specs.reserve(projections.size());
    for (auto& [name, exprStr] : projections) {
        std::istringstream ss(exprStr);
        NQumir::NAst::NCore::TTokenStream tokens(ss);
        NQumir::NAst::NCore::TParser parser;
        auto parsed = parser.Parse(tokens);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        specs.push_back({std::move(name), std::move(*parsed)});
    }
    return std::make_shared<TProjectOperator>(std::move(input), std::move(specs));
}

} // namespace NQdb
