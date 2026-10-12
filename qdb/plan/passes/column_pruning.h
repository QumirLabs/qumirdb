#pragma once

#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/operator.h>

#include <string>
#include <unordered_set>

namespace NQdb {

using TColumnSet = std::unordered_set<std::string>;

// Top-down pass: narrows TFunctionType::ParamTypes[0] of each operator to only
// the columns it actually needs, then calls SetRestrictedColumns on source nodes
// to restrict physical column reads from parquet.
//
// Must be called after AnnotateTypes (operators must have TFunctionType set).
void ApplyColumnPruning(const TOperatorPtr& root);

// `explicitRootDemand` null -> seed from the root's full output; non-null (even
// empty) -> seed exactly from it. Each TCteRef accumulates its refcount and the
// columns its consumer needs into `usage`.
void ApplyColumnPruning(
    const TOperatorPtr& root, const TColumnSet* explicitRootDemand, TCteUsageMap* usage);

TCteUsageMap PropagateCteDemands(const TOperatorPtr& main);

} // namespace NQdb
