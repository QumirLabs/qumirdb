#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/ops/operator.h>

namespace NQdb {

bool HasUnresolvedSqlBindings(const TOperatorPtr& plan);

// Resolves SQL positions, output aliases and grouping expressions before the
// ordinary optimizer pipeline. All resulting keys are column names.
void ResolveSqlBindings(TOperatorPtr& plan, const NKernel::TAnnotationContext& context = {});

} // namespace NQdb
