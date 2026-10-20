#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Whether the plan or one of its CTE definitions still carries SQL references.
bool HasUnresolvedSqlBindings(const TOperatorPtr& plan);

// Resolves SQL names, stars, output aliases, grouping, ORDER BY and set-operation
// keys against typed inputs before the ordinary optimizer pipeline. All
// resulting references are column names.
void ResolveSqlBindings(TOperatorPtr& plan, const NKernel::TAnnotationContext& context = {});

} // namespace NQdb
