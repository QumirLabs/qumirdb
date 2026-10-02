#include <qdb/plan/pipeline.h>

#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/estimate_stats.h>
#include <qdb/plan/passes/flatten_joins.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/late_materialization.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/cte_reuse.h>
#include <qdb/plan/passes/top_sort.h>
#include <qdb/plan/passes/push_limit.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/passes/row_group_predicate.h>
#include <qdb/plan/passes/runtime_filters.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/plan_print.h>

#include <cstdlib>
#include <iostream>

namespace NQdb {

namespace {

void OptimizeOne(TOperatorPtr& plan, TPlanPassOptions& options) {
    // QDB_DUMP_PASSES=1 dumps the plan after each pass to stderr (debugging).
    const bool dump = std::getenv("QDB_DUMP_PASSES") != nullptr;
    auto stage = [&](const char* name) {
        if (dump) {
            std::cerr << "\n===== after " << name << " =====\n";
            PrintPlanTree(std::cerr, plan);
        }
    };
    if (dump) { std::cerr << "\n===== initial =====\n"; PrintPlanTree(std::cerr, plan); }

    BindLateMaterializationSources(plan);
    AssignSourceAliases(plan); stage("AssignSourceAliases");
    QualifyColumns(plan); stage("QualifyColumns");
    AnnotateTypes(plan, options.Annotation);
    if (options.EnableCbo) {
        plan = FlattenInnerJoins(plan); stage("FlattenInnerJoins");
        AnnotateTypes(plan, options.Annotation); // re-annotate: inner-join regions were reassociated
    }
    plan = PushDownPredicates(plan); stage("PushDownPredicates");
    AnnotateTypes(plan, options.Annotation); // re-annotate: pushdown moved filters onto leaves
    EstimateStats(plan); // leaf cardinalities for cost-based ReorderJoins
    plan = ReorderJoins(
        plan,
        options.EnableCbo,
        options.Diagnostics ? &options.Diagnostics->JoinReorder : nullptr);
    stage("ReorderJoins");
    AnnotateTypes(plan, options.Annotation); // re-annotate: reordering rebuilt the join tree
    plan = ExtractEquiJoins(plan); stage("ExtractEquiJoins");
    AnnotateTypes(plan, options.Annotation); // re-annotate: equi-join extraction adds/removes nodes
    plan = PushDownSemiJoins(plan); stage("PushDownSemiJoins");
    AnnotateTypes(plan, options.Annotation); // re-annotate: semi pushdown restructured joins
    plan = PushDownLimits(plan);
    AnnotateTypes(plan, options.Annotation); // re-annotate: limit moved below strip projections
    plan = ApplyTopSort(plan);
    AnnotateTypes(plan, options.Annotation); // re-annotate: top-sort replaces limit(sort(...))
    CoerceSetOpBranches(plan); stage("CoerceSetOpBranches");
    AnnotateTypes(plan, options.Annotation); // re-annotate: coercion added cast projections on union branches
    ApplyColumnPruning(plan); stage("ApplyColumnPruning");

    // Pruning removes dead null-extended projections before the anti-join rule
    // checks demand. The narrower join schema needs typing/pruning again.
    if (RewriteOuterJoinAsAnti(plan)) {
        stage("RewriteOuterJoinAsAnti");
        AnnotateTypes(plan, options.Annotation);
        plan = PushDownSemiJoins(plan);
        AnnotateTypes(plan, options.Annotation);
        ApplyColumnPruning(plan);
    }

    EstimateStats(plan);
}

template <class TFn>
void ForEachPlan(TOperatorPtr& main, TFn&& fn, bool includeMain = true) {
    for (const auto& def : CollectCteDefinitions(main)) {
        fn(def->Plan, def.get());
    }
    if (includeMain) {
        fn(main, nullptr);
    }
}

} // namespace

void ApplyPlanPasses(TOperatorPtr& plan, TPlanPassOptions options) {
    ForEachPlan(plan, [&](TOperatorPtr& current, TCteDefinition* def) {
        OptimizeOne(current, options);
        if (def) {
            def->OutputType = current->OutputColumns();
        }
    });
    PushConsumerPredicatesIntoDefinitions(plan, options.Annotation);
    auto usage = PropagateCteDemands(plan);

    // CTE output demand is only known after PropagateCteDemands. Apply the
    // width-sensitive rewrite now so it costs and fetches the pruned schema.
    TLateMaterializationDiagnostics lateDiagnostics;
    int lateDiagnosticsRank = -1;
    ForEachPlan(plan, [&](TOperatorPtr& current, TCteDefinition* def) {
        TLateMaterializationDiagnostics local;
        current = ApplyLateMaterialization(
            current, options.LateMaterialization, &local);
        AnnotateTypes(current, options.Annotation);

        // Re-annotation restores schema-preserving operators (notably filters)
        // to their full input schema. Re-apply the already-computed demand so
        // predicate-only columns stay out of the output and a newly-added row
        // locator reaches the physical source restriction.
        const TColumnSet* rootDemand = nullptr;
        if (def) {
            auto it = usage.find(def);
            if (it == usage.end()) {
                throw std::runtime_error(
                    "cte demand: optimized definition has no usage record");
            }
            rootDemand = &it->second.RequiredOutputs;
        }
        ApplyColumnPruning(current, rootDemand, nullptr);
        if (def) {
            def->OutputType = current->OutputColumns();
        }
        EstimateStats(current);

        const int rank = local.Applied ? 3
            : local.Considered ? 2
            : !local.Reason.empty() ? 1
            : 0;
        if (rank > lateDiagnosticsRank) {
            lateDiagnostics = std::move(local);
            lateDiagnosticsRank = rank;
        }
    });
    if (options.Diagnostics) {
        options.Diagnostics->LateMaterialization = std::move(lateDiagnostics);
    }
    auto decisions = ChooseCteReuse(usage);
    plan = ApplyCteReuse(std::move(plan), decisions);
    ForEachPlan(plan, [](TOperatorPtr& current, TCteDefinition*) {
        AttachRowGroupPredicates(current);
    });
    {
        // Runs last, so the pairing it records survives every restructuring
        // pass. One counter across every plan of the query: after ApplyCteReuse
        // a shared definition lives in its own materialization plan, which
        // ForEachPlan no longer reaches, and two joins must never share an id.
        uint32_t nextFilterId = 1;
        for (const auto& materialization : CollectMaterializations(plan)) {
            AttachRuntimeFilters(materialization.Materialization->Plan,
                nextFilterId, options.ForceRuntimeFilters);
        }
        ForEachPlan(plan, [&](TOperatorPtr& current, TCteDefinition*) {
            AttachRuntimeFilters(current, nextFilterId, options.ForceRuntimeFilters);
        });
    }
    if (std::getenv("QDB_DUMP_PASSES") != nullptr) {
        std::cerr << "\n===== after ApplyCteReuse =====\n";
        PrintPlanTreeWithCtes(std::cerr, plan);
    }
}

} // namespace NQdb
