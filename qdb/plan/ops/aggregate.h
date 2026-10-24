#pragma once

#include <qdb/plan/ops/operator.h>

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
    bool EmptyIsZero = false;   // merging partial COUNT states with SUM
};

class TAggregateOperator : public IOperator {
public:
    static constexpr const char* OpId = "aggregate";

    TAggregateOperator(TOperatorPtr input, std::vector<std::string> groupKeys,
        std::vector<TAggregateSpec> aggs, bool global = false);

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override;
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
    // A global aggregate owns one group even when no input row arrives.
    bool IsGlobal() const { return Global_; }
    // Ids of the groups that exist over an empty input: the global group (id 1, the
    // constant `__group__` key), or the sets of GroupingSets() that group by no key
    // (ROLLUP's and CUBE's grand total), identified by their index, which is the
    // `__grouping_id__` the executor feeds. Their aggregates are NULL (COUNT 0) there.
    std::vector<int64_t> EmptyInputGroups() const;
    const std::vector<TAggregateSpec>& Aggs() const { return Aggs_; }
    std::vector<TAggregateSpec>& MutableAggs() { return Aggs_; }

    // Each set holds indices into GroupKeys_; keys outside a set are NULL in its
    // output. Empty = plain single-set aggregate over all keys (non-nullable).
    const std::vector<std::vector<size_t>>& GroupingSets() const { return GroupingSets_; }
    std::vector<std::vector<size_t>>& MutableGroupingSets() { return GroupingSets_; }

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
    bool Global_ = false;
    std::vector<std::vector<size_t>> GroupingSets_;
};

// The result type of aggregate `func` over an argument of `argType`, which is
// null for count(*).
NQumir::NAst::TTypePtr AggregateResultType(
    const std::string& func,
    const NQumir::NAst::TTypePtr& argType);

// Output schema = group key columns (types from inputSchema) followed by one
// column per aggregate (type derived from Func/Arg). Used both by the
// constructor and by AnnotateTypes (after column pruning narrows inputSchema).
// nullableKeys: GROUPING SETS make key columns nullable (NULL in sets that omit them).
NQumir::NAst::TTypePtr ComputeAggregateOutputType(
    const NQumir::NAst::TTypePtr& inputSchema,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs,
    bool nullableKeys = false,
    bool global = false);

// aggs: (output_name, func, arg_expr_string) triples; arg_expr_string empty for count(*)
std::expected<TOperatorPtr, NQumir::TError>
MakeAggregate(TOperatorPtr input, std::vector<std::string> groupKeys,
    std::vector<std::tuple<std::string, std::string, std::string>> aggs);

} // namespace NQdb
