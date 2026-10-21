#include <qdb/plan/ops/filter.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>

namespace NQdb {

TFilterOperator::TFilterOperator(TOperatorPtr input, NQumir::NAst::TExprPtr predicate)
    : Input_(std::move(input))
    , Predicate_(std::move(predicate))
{
    // Filter is schema-preserving: output = required = input's output schema.
    auto schema = Input_->OutputColumns();
    Type = std::make_shared<NQumir::NAst::TFunctionType>(
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
MakeFilter(TOperatorPtr input, const std::string& predicate) {
    std::istringstream ss(predicate);
    NQumir::NAst::NCore::TTokenStream tokens(ss);
    NQumir::NAst::NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::make_shared<TFilterOperator>(std::move(input), std::move(*parsed));
}

} // namespace NQdb
