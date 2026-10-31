#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/ops/dependent_join.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/operator.h>

#include <qumir/error.h>
#include <qumir/parser/ast.h>

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_set>
#include <vector>

namespace NQdb {
namespace NBuild {

// A built relation and the SQL name of each output column, qualified by its FROM
// item where one applies. An empty label hides a column from SQL, such as the
// marker of a mark join. Physical names only need to be unique where relations
// meet. The operations below build relations from relations and keep the labels
// in step with the physical columns.
struct TRelation {
    TOperatorPtr Plan;
    std::vector<std::string> Labels;
    // Labels SQL reaches only qualified: the inputs of a USING column.
    std::unordered_set<std::string> QualifiedOnly;
};

// The physical names of the output columns of a plan.
std::vector<std::string> ColumnNames(const TOperatorPtr& plan);

// Types an operator built over typed inputs. Kernel type annotation reports
// errors as exceptions; the builder returns them as values.
std::expected<void, NQumir::TError> Annotate(
    const TOperatorPtr& plan,
    const NKernel::TAnnotationContext& annotation);

// Renames the output columns of a plan: the plan's own top projection in place,
// any other plan with a renaming projection.
std::expected<TOperatorPtr, NQumir::TError> RenameOutputs(
    TOperatorPtr plan,
    const std::vector<std::string>& names,
    const NKernel::TAnnotationContext& annotation);

// FROM items may expose equal physical names, e.g. two unaliased derived tables.
// The right one gets other names; labels stay.
std::expected<TRelation, NQumir::TError> SeparateRight(
    const TRelation& left,
    TRelation right,
    const NKernel::TAnnotationContext& annotation);

// The labels of a plan built over `input`: a column that keeps the name of an
// input column keeps its label, any other column is hidden.
TRelation CarryLabels(const TRelation& input, TOperatorPtr plan);

// Joins two relations; a semi or anti join keeps the labels of the input it
// outputs.
TRelation JoinRelations(
    TRelation left,
    TRelation right,
    std::vector<TJoinKey> keys,
    EJoinType type,
    NQumir::NAst::TExprPtr residual);

// Attaches `right` to `tree`, the tree of query block `scope`, with a dependent
// join. A mark or single join adds one hidden column, the marker or the value.
std::expected<void, NQumir::TError> AttachDependent(
    TRelation& tree,
    uint32_t scope,
    TRelation right,
    EDependentJoinKind kind,
    NQumir::NAst::TExprPtr predicate,
    std::string marker,
    const NQumir::TLocation& location,
    const NKernel::TAnnotationContext& annotation);

} // namespace NBuild
} // namespace NQdb
