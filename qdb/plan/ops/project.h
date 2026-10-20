#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/sql_bindings.h>

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
    // Empty for scalar projections and unbound default colN struct names.
    std::vector<std::string> ColumnAliases;
    // True for an unnamed SQL expression (`SELECT a + b`) until first typing.
    // Initial typing may shift its colN name when an earlier projection expands.
    // For a struct expression, Name anchors the group at its first flattened column.
    bool ImplicitName = false;
    std::optional<std::string> StarPrefix;
};

// Number of output columns occupied after flattening a struct-valued projection.
size_t FlattenedProjectionArity(const TProjectionSpec& projection);

class TProjectOperator : public IOperator {
public:
    static constexpr const char* OpId = "project";

    TProjectOperator(TOperatorPtr input, std::vector<TProjectionSpec> projections);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
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

    const std::shared_ptr<const TSelectBinding>& SelectBinding() const { return SelectBinding_; }
    void SetSelectBinding(std::shared_ptr<const TSelectBinding> binding) { SelectBinding_ = std::move(binding); }
    const std::optional<TOutputAliasesBinding>& OutputAliases() const { return OutputAliases_; }
    std::optional<TOutputAliasesBinding>& MutableOutputAliases() { return OutputAliases_; }

    std::span<const TOperatorPtr> Inputs() const override {
        return std::span<const TOperatorPtr>(&Input_, 1);
    }

    std::span<TOperatorPtr> MutableInputs() override {
        return std::span<TOperatorPtr>(&Input_, 1);
    }

private:
    TOperatorPtr Input_;
    std::vector<TProjectionSpec> Projections_;
    std::shared_ptr<const TSelectBinding> SelectBinding_;
    std::optional<TOutputAliasesBinding> OutputAliases_;
};

// projections: list of (output_name, expression_string) pairs
std::expected<TOperatorPtr, NQumir::TError>
MakeProject(TOperatorPtr input, std::vector<std::pair<std::string, std::string>> projections);

} // namespace NQdb
