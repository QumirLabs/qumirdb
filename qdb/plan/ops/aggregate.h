#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/sql_bindings.h>

#include <qumir/error.h>

#include <expected>
#include <string>
#include <tuple>
#include <vector>

namespace NQdb {

struct TAggregateSpec {
    std::string Name;            // output column name
    std::string Func;            // "count" | "sum" | "min" | "max"
    NQumir::NAst::TExprPtr Arg;  // parsed, unannotated; nullptr for count(*)
};

class TAggregateOperator : public IOperator {
public:
    static constexpr const char* OpId = "aggregate";

    TAggregateOperator(TOperatorPtr input, std::vector<std::string> groupKeys, std::vector<TAggregateSpec> aggs);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    // Aggregate defines a new schema: child only needs group keys + agg args.
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t, const std::unordered_set<std::string>&) const override {
        return ComputeReferencedColumns();
    }
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }
    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    TOperatorPtr& MutableInput() { return Input_; }
    const std::vector<std::string>& GroupKeys() const { return GroupKeys_; }
    std::vector<std::string>& MutableGroupKeys() { return GroupKeys_; }
    const std::vector<TAggregateSpec>& Aggs() const { return Aggs_; }
    std::vector<TAggregateSpec>& MutableAggs() { return Aggs_; }

    // Each set holds indices into GroupKeys_; keys outside a set are NULL in its
    // output. Empty = plain single-set aggregate over all keys (non-nullable).
    const std::vector<std::vector<size_t>>& GroupingSets() const { return GroupingSets_; }
    std::vector<std::vector<size_t>>& MutableGroupingSets() { return GroupingSets_; }

    const std::optional<TAggregateBinding>& Binding() const { return Binding_; }
    std::optional<TAggregateBinding>& MutableBinding() { return Binding_; }
    bool GroupsAllColumns() const { return GroupsAllColumns_; }
    void SetGroupsAllColumns(bool value) { GroupsAllColumns_ = value; }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(&Input_, 1);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(&Input_, 1);
    }

private:
    TOperatorPtr Input_;
    std::vector<std::string> GroupKeys_;
    std::vector<TAggregateSpec> Aggs_;
    std::vector<std::vector<size_t>> GroupingSets_;
    std::optional<TAggregateBinding> Binding_;
    bool GroupsAllColumns_ = false;
};

// Output schema = group key columns (types from inputSchema) followed by one
// column per aggregate (type derived from Func/Arg). Used both by the
// constructor and by AnnotateTypes (after column pruning narrows inputSchema).
// nullableKeys: GROUPING SETS make key columns nullable (NULL in sets that omit them).
NQumir::NAst::TTypePtr ComputeAggregateOutputType(
    const NQumir::NAst::TTypePtr& inputSchema,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs,
    bool nullableKeys = false);

// aggs: (output_name, func, arg_expr_string) triples; arg_expr_string empty for count(*)
std::expected<TOperatorPtr, NQumir::TError>
MakeAggregate(TOperatorPtr input, std::vector<std::string> groupKeys,
    std::vector<std::tuple<std::string, std::string, std::string>> aggs);

} // namespace NQdb
