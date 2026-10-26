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

// For each definition, pushes its consumers' predicates into its plan. The
// filter of a single reference moves into the definition, as for a derived
// table. With several references, the disjunction of their predicates over the
// CTE output columns is pushed and every consumer keeps its filter. The
// disjunction is row-superset: it drops only rows no consumer keeps, so the
// materialized producer stays correct for every reference. Pushed only when
// every reference constrains the column. Consumers go before the definitions
// they read, so a pushed predicate reaches nested definitions. Delegates the
// descent (branch pruning, const-folding) to PushDownPredicates. Run after
// main+definition optimization, before ChooseCteReuse.
void PushConsumerPredicatesIntoDefinitions(
    TOperatorPtr& main,
    const NKernel::TAnnotationContext& context,
    TConstFolder& folder);

// >=2 references -> Materialize, else Inline.
TCteReuseDecisions ChooseCteReuse(const TCteUsageMap& usage);

// Realizes each TCteRef per its decision: Inline splices a private clone,
// Materialize leaves a TCteConsumer sharing one TCteMaterialization.
TOperatorPtr ApplyCteReuse(TOperatorPtr plan, const TCteReuseDecisions& decisions);

} // namespace NQdb
