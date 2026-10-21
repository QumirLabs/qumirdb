#pragma once

#include <qdb/plan/ops/operator.h>

#include <qumir/error.h>

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

namespace NQdb {

struct TProjectionSpec {
    std::string Name;
    NQumir::NAst::TExprPtr Expression; // parsed, unannotated
    // Flattened group names, explicit from `AS (x, y, z)` or fixed at first typing.
    // Empty for scalar projections and, until first typing, default colN names.
    std::vector<std::string> ColumnAliases;
    // True for an unnamed SQL expression (`SELECT a + b`) until first typing.
    // Initial typing may shift its colN name when an earlier projection expands.
    // For a struct expression, Name anchors the group at its first flattened column.
    bool ImplicitName = false;
    // SELECT DISTINCT is built before struct arity is known. Reject an unaliased
    // struct during typing instead of silently deduplicating only its first field.
    bool RejectUnaliasedStructForDistinct = false;
};

// Number of output columns occupied after flattening a struct-valued projection.
size_t FlattenedProjectionArity(const TProjectionSpec& projection);

class TProjectOperator : public IOperator {
public:
    static constexpr const char* OpId = "project";

    TProjectOperator(TOperatorPtr input, std::vector<TProjectionSpec> projections);

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override;
    // Project defines a new schema: child only needs project's own refs.
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t, const std::unordered_set<std::string>&) const override {
        return ComputeReferencedColumns();
    }
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }
    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    TOperatorPtr& MutableInput() { return Input_; }
    const std::vector<TProjectionSpec>& Projections() const { return Projections_; }
    std::vector<TProjectionSpec>& MutableProjections() { return Projections_; }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(&Input_, 1);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(&Input_, 1);
    }

private:
    TOperatorPtr Input_;
    std::vector<TProjectionSpec> Projections_;
};

// projections: list of (output_name, expression_string) pairs
std::expected<TOperatorPtr, NQumir::TError>
MakeProject(TOperatorPtr input, std::vector<std::pair<std::string, std::string>> projections);

} // namespace NQdb
