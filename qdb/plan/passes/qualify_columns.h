#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Pre-order: set TSourceOperator::Alias_ based on file path stem.
// Sources with the same stem get numeric suffixes: nation_0, nation_1, ...
// An explicit alias already set via sexp is respected and excluded from numbering.
void AssignSourceAliases(const TOperatorPtr& root);

// Bottom-up: rename all column references to "alias.col" form.
// - Renames TStructType fields in TSourceOperator::Type
// - Updates TJoinKey::Left/Right
// - Walks TIdentExpr leaves in filter predicates, project expressions,
//   join residual predicates, and aggregate group keys / agg args
void QualifyColumns(const TOperatorPtr& root);

} // namespace NQdb
