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
// The first call fixes projection output names; later calls keep them, so it is
// safe to call again after plan modifications.
void AnnotateTypes(
    const TOperatorPtr& root,
    const NKernel::TAnnotationContext& context = {});

// Applies the same type rules to one node whose inputs are already typed.
void AnnotateOperatorType(
    const TOperatorPtr& root,
    const NKernel::TAnnotationContext& context = {});

// Inserts cast projections on UNION ALL branches so every branch produces the unified
// column layout. Run after AnnotateTypes (needs the union's unified output type), then
// re-annotate.
void CoerceSetOpBranches(
    const TOperatorPtr& root,
    const NKernel::TAnnotationContext& context = {});

// Equi-join keys compare equal bytes, so INTEGER = BIGINT cannot share a key. Casts the
// narrower integer key of each join to the common integer type in a projection below
// that side and re-points the key at the casted column. Run after AnnotateTypes, then
// re-annotate.
void CoerceJoinKeys(const TOperatorPtr& root);

} // namespace NQdb
