#pragma once

#include <qdb/plan/ops/operator.h>

#include <string>
#include <unordered_set>

namespace NQdb {

// Whether `predicate` cannot be TRUE on a row whose `columns` are all NULL,
// whatever the other columns hold: the predicate is null-rejecting for them.
// Conservative: false whenever the analysis cannot prove it.
bool RejectsNulls(
    const NQumir::NAst::TExprPtr& predicate,
    const std::unordered_set<std::string>& columns);

// Outer-join simplification: narrows an outer join whose null-extended rows a
// null-rejecting predicate above it would drop (LEFT or RIGHT to INNER, FULL to
// LEFT, RIGHT or INNER). Requires qualified columns and annotated types.
// Returns whether the plan changed; re-annotate after a rewrite.
bool SimplifyOuterJoins(TOperatorPtr& root);

} // namespace NQdb
