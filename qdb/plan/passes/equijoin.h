#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

class TConstFolder;

// Rewrites empty-key inner joins into equi-joins by lifting equalities from the
// surrounding WHERE/ON predicates into join keys (transitive via equivalence
// classes). Runs after QualifyColumns and AnnotateTypes. Returns the new root
// (the top filter may be consumed).
TOperatorPtr ExtractEquiJoins(TOperatorPtr root, TConstFolder& folder);

// Both passes push predicates into UNION ALL branches; `folder` folds a
// predicate over a branch's constant columns to drop the branch it rejects.
TOperatorPtr PushDownPredicates(TOperatorPtr root, TConstFolder& folder);

// Removes LEFT/RIGHT JOIN + IS NULL on a null-extended equijoin key when that
// side's columns are not needed above the filter. RIGHT is swapped to LeftAnti.
// Requires annotated types and extracted keys.
// Returns whether the plan changed; re-annotate and prune after a rewrite.
bool RewriteOuterJoinAsAnti(TOperatorPtr& root);

} // namespace NQdb
