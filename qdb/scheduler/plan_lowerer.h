#pragma once

#include <qdb/io/io.h>
#include <qdb/exec/plan.h>
#include <qdb/kernel/generated.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/settings.h>

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace NQdb {
class TExternalCatalogSnapshot;
class TAggregateOperator;
class IRuntimeFilterBindingFactory;
namespace NScheduler {

// A plan lowered into a scheduler task graph, without a terminal sink attached.
// FinalGather and Producers point into Graph; a sink is attached to FinalGather
// (one edge per producer lane) before the graph is run.
//
// Kernels holds every kernel generated during lowering, as ASTs with unbound
// slots. ExecStageId is their stable association with graph stages; Stage is
// diagnostic text only. A finalizer must bind or compile them before a consumer
// executes the lowered plan.
struct TLoweredPlan {
    std::unique_ptr<TTaskGraph> Graph;
    NQumir::NAst::TTypePtr OutputType;
    IConnection* FinalGather = nullptr;
    std::vector<TTaskNode*> Producers;
    size_t Lanes = 0;
    std::vector<TGeneratedKernel> Kernels;
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog;
    // Stable, exporter-neutral executable stages produced by the same lowering
    // that creates Graph. Debug labels are deliberately not identities.
    std::vector<TLoweredExecStage> ExecStages;
};

// Choose hash-shuffle fanout for a plain grouped aggregate. Explicit shuffle
// settings win; auto mode may use aggregate statistics to exceed the
// conservative join/default fanout.
size_t ChooseAggregatePartitionCount(
    const TAggregateOperator& aggregate,
    size_t inputLanes,
    const TSettings& settings);

// Whether a grouped aggregate should be split into local partial and final
// phases around a repartition exchange.
bool ShouldUsePartialAggregate(
    const TAggregateOperator& aggregate,
    const TSettings& settings);

// Lower a logical plan into a scheduler graph (no terminal sink yet).
// Kernels are generated but not compiled.
// A custom filter factory can bind query-scoped filter ids to remote endpoints.
TLoweredPlan LowerPlanToGraph(
    const TOperatorPtr& root,
    TSettings settings,
    std::ostream* diagnostics,
    std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog = nullptr,
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings = nullptr);

// Attach a terminal sink that writes every output rowset to `sink`, then run the
// graph on the configured scheduler. Returns false and sets `error` on failure.
bool RunPlanIntoSink(
    TLoweredPlan lowered,
    ISink& sink,
    TSettings settings,
    std::ostream* diagnostics,
    std::string* error);

} // namespace NScheduler
} // namespace NQdb
