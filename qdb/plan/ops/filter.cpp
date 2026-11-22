#include <qdb/plan/ops/filter.h>

#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>

namespace NQdb {

TFilterOperator::TFilterOperator(NQumir::NAst::TTypeFactory& types, TOperatorPtr input, NQumir::NAst::TExprPtr predicate)
    : Input_(std::move(input))
    , Predicate_(std::move(predicate))
{
    // Filter is schema-preserving: output = required = input's output schema.
    auto schema = Input_->OutputColumns();
    Type = types.MakeFunction(
        std::vector<NQumir::NAst::TTypePtr>{schema},
        schema);
}

std::vector<TColumnReference> TFilterOperator::ColumnReferences() const {
    std::vector<TColumnReference> references;
    AppendColumnReferences(references, Predicate_);
    return references;
}

const std::string TFilterOperator::ToString() const {
    using namespace NQumir::NAst::NCore;
    return "(rel filter " + Input_->ToString() + " " + PrintAst(Predicate_) + ")";
}

std::expected<TOperatorPtr, NQumir::TError>
MakeFilter(NQumir::NAst::TTypeFactory& types, TOperatorPtr input, const std::string& predicate) {
    std::istringstream ss(predicate);
    NQumir::NAst::NCore::TTokenStream tokens(ss);
    NQumir::NAst::NCore::TParser parser(types);
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return NQumir::MakeIntrusive<TFilterOperator>(types, std::move(input), std::move(*parsed));
}

} // namespace NQdb
