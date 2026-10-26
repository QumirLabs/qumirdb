#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/sql/ast.h>

#include <qumir/error.h>

#include <expected>

namespace NQdb {
namespace NBuild {

// Builds the canonical plan of a SQL query for the new decorrelation pipeline
// (Neumann, Leis and Kemper, "The Complete Story of Joins (in HyPer)", BTW
// 2017, section 3.4). It only removes SQL semantics: names are bound, a FROM
// item that reads the items before it is attached with a dependent join, and
// every subquery becomes a dependent mark or single join over the tree of its
// query block, the expression reading the marker or scalar column. Nothing is
// decorrelated or otherwise rewritten; that is left to explicit passes.
// Constructs outside SELECT ... FROM ... WHERE ... GROUP BY ... HAVING ...
// ORDER BY ... LIMIT over tables, joins, derived tables, CTEs and subqueries are
// rejected. Like BuildPlan, it binds the expression nodes of `query` in place.
std::expected<TOperatorPtr, NQumir::TError> BuildCanonicalPlan(
    const NQdb::NSql::TSqlNodePtr& query,
    const TTableSourceFactory& sources,
    const NKernel::TAnnotationContext& annotation = {});

} // namespace NBuild
} // namespace NQdb
