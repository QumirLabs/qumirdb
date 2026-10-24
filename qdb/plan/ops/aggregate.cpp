#include <qdb/plan/ops/aggregate.h>

#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>

namespace NQdb {

using namespace NQumir::NAst;

TTypePtr AggregateResultType(const std::string& func, const TTypePtr& argType) {
    if (func == "count") {
        return std::make_shared<TIntegerType>(); // i64
    }
    if (argType) {
        // Nullable must be unwrapped before the integer check, else a nullable
        // integer arg falls through and the i32-width output is read against the
        // reducer's i64 state buffer (every other output element reads a high 0).
        const bool nullable = IsNullableType(argType);
        auto inner = nullable ? UnwrapNullableType(argType) : argType;
        if (inner && inner->TypeName() == TIntegerType::TypeId) {
            // i64 state for every integer aggregate, incl. min/max over narrow ints.
            TTypePtr i64Type = std::make_shared<TIntegerType>();
            return nullable ? std::make_shared<TNullable>(std::move(i64Type)) : i64Type;
        }
    }
    // Non-integer reducers are not connected yet; preserve the declared type.
    return argType;
}

TTypePtr ComputeAggregateOutputType(
    const TTypePtr& inputSchema,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs,
    bool nullableKeys,
    bool global)
{
    auto* inputStruct = static_cast<TStructType*>(inputSchema.get());
    std::vector<std::pair<std::string, TTypePtr>> outFields;
    // The kernel emits the grouping-set id as the leading key so masked-NULL and
    // real-NULL groups stay distinct; downstream references keys by name.
    if (nullableKeys) {
        outFields.emplace_back("__grouping_id__", std::make_shared<TIntegerType>(TIntegerType::I32));
    }
    for (const auto& key : groupKeys) {
        auto type = FieldType(inputStruct, key);
        if (nullableKeys && type && !IsNullableType(type)) {
            type = std::make_shared<TNullable>(std::move(type));
        }
        outFields.emplace_back(key, std::move(type));
    }
    for (const auto& agg : aggs) {
        TTypePtr argType;
        if (agg.Arg) {
            if (auto ident = TMaybeNode<TIdentExpr>(agg.Arg)) {
                argType = FieldType(inputStruct, ident.Cast()->Name);
            }
        }
        auto resultType = AggregateResultType(agg.Func, argType);
        if (global && agg.Func != "count" && !agg.EmptyIsZero &&
            resultType && !IsNullableType(resultType))
        {
            resultType = std::make_shared<TNullable>(std::move(resultType));
        }
        outFields.emplace_back(agg.Name, std::move(resultType));
    }
    return std::make_shared<TStructType>(std::move(outFields));
}

TAggregateOperator::TAggregateOperator(TOperatorPtr input, std::vector<std::string> groupKeys,
    std::vector<TAggregateSpec> aggs, bool global)
    : Input_(std::move(input))
    , GroupKeys_(std::move(groupKeys))
    , Aggs_(std::move(aggs))
    , Global_(global)
{
    auto inputSchema = Input_->OutputColumns();
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{inputSchema},
        ComputeAggregateOutputType(inputSchema, GroupKeys_, Aggs_, !GroupingSets_.empty(), Global_));
}

std::vector<TColumnReference> TAggregateOperator::ColumnReferences() const {
    std::vector<TColumnReference> references;
    for (const auto& key : GroupKeys_) {
        references.push_back({.Name = key});
    }
    for (const auto& agg : Aggs_) {
        AppendColumnReferences(references, agg.Arg);
    }
    return references;
}

const std::string TAggregateOperator::ToString() const {
    std::string s = "(rel aggregate " + Input_->ToString();
    s += " (keys";
    for (const auto& key : GroupKeys_) {
        s += " " + key;
    }
    s += ")";
    for (const auto& agg : Aggs_) {
        s += " (agg " + agg.Name + " " + agg.Func;
        if (agg.Arg) {
            s += " " + NQumir::NAst::NCore::PrintAst(agg.Arg);
        }
        s += ")";
    }
    return s + ")";
}

std::expected<TOperatorPtr, NQumir::TError>
MakeAggregate(TOperatorPtr input, std::vector<std::string> groupKeys,
    std::vector<std::tuple<std::string, std::string, std::string>> aggs)
{
    std::vector<TAggregateSpec> specs;
    specs.reserve(aggs.size());
    for (auto& [name, func, exprStr] : aggs) {
        TExprPtr arg;
        if (!exprStr.empty()) {
            std::istringstream ss(exprStr);
            NQumir::NAst::NCore::TTokenStream tokens(ss);
            NQumir::NAst::NCore::TParser parser;
            auto parsed = parser.Parse(tokens);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            arg = std::move(*parsed);
        }
        specs.push_back({std::move(name), std::move(func), std::move(arg)});
    }
    return std::make_shared<TAggregateOperator>(std::move(input), std::move(groupKeys), std::move(specs));
}

} // namespace NQdb
