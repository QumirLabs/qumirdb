#pragma once

#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/passes/estimate_stats.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace NQdb {

namespace NKernel {
struct TAnnotationContext;
}

enum class ECteReuseMode {
    Inline,
    Materialize,
};

struct TCteReuseDecision {
    ECteReuseMode Mode;
    std::unordered_set<std::string> RequiredOutputs; // for the 5b boundary projection
};
using TCteReuseDecisions = std::unordered_map<TCteDefinition*, TCteReuseDecision>;

// For each definition, pushes the disjunction of its references' predicates over
// the CTE output columns into the shared plan. The disjunction is row-superset:
// it drops only rows no consumer keeps, so the materialized producer stays
// correct for every reference. Pushed only when every reference constrains the
// column; delegates the descent (branch pruning, const-folding) to
// PushDownPredicates. Run after main+definition optimization, before ChooseCteReuse.
void PushConsumerPredicatesIntoDefinitions(
    const TOperatorPtr& main,
    const NKernel::TAnnotationContext& context,
    TConstFolder& folder);

// >=2 references -> Materialize, else Inline.
TCteReuseDecisions ChooseCteReuse(const TCteUsageMap& usage);

// Realizes each TCteRef per its decision: Inline splices a private clone,
// Materialize leaves a TCteConsumer sharing one TCteMaterialization.
TOperatorPtr ApplyCteReuse(TOperatorPtr plan, const TCteReuseDecisions& decisions);

} // namespace NQdb
