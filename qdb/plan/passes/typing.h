#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Bottom-up pass: assigns TFunctionType([required], output) to every operator.
//   Source : fun()          -> struct(all columns from schema)
//   Filter : fun(input_schema) -> input_schema
//   Project: fun(input_schema) -> struct(projection specs)
//
// ParamTypes[0] = full input schema initially; narrowed by ApplyColumnPushdown.
// Idempotent - safe to call again after plan modifications.
void AnnotateTypes(
    const TOperatorPtr& root,
    const NKernel::TAnnotationContext& context = {});

// Inserts cast projections on UNION ALL branches so every branch produces the unified
// column layout. Run after AnnotateTypes (needs the union's unified output type), then
// re-annotate.
void CoerceSetOpBranches(const TOperatorPtr& root);

} // namespace NQdb
