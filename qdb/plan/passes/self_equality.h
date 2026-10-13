#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Rewrites `col == col` in filter predicates: TRUE for a non-nullable column,
// `col IS NOT NULL` for a nullable one. Decorrelation leaves such comparisons
// where a correlation equality used to be. Only the AND/OR spine of a predicate
// is rewritten, where a filter treats NULL and FALSE alike; float columns are
// kept because NaN != NaN. A filter that becomes TRUE is removed.
// Requires annotated types. Returns the possibly replaced root.
TOperatorPtr SimplifySelfEquality(TOperatorPtr root);

} // namespace NQdb
