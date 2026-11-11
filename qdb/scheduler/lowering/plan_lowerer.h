#pragma once

#include <qdb/io/io.h>
#include <qdb/exec/plan.h>
#include <qdb/kernel/generated.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/settings/settings.h>
#include <qdb/scheduler/spec/graph_spec.h>

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NQdb {
class TExternalCatalogSnapshot;
struct TRuntimeContext;
class TAggregateOperator;
class IRuntimeFilterBindingFactory;
class IRuntimeFilterProbe;
namespace NScheduler {

struct TRuntimeFilterDiagnostic {
    uint32_t Id = 0;
    std::shared_ptr<IRuntimeFilterProbe> Probe;
    bool AtSource = false;
};

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
    std::vector<TRuntimeFilterDiagnostic> RuntimeFilters;
    // Spec records indexed by node index; empty for node kinds the graph spec
    // does not describe yet.
    std::vector<std::optional<TNodeSpec>> NodeSpecs;
    // Shared by Kernels; the graph spec carries its regexes.
    std::shared_ptr<TRuntimeContext> RuntimeContext;
    // The graph spec's CTE tables, which the CTE records index.
    std::vector<TCteSpoolSpec> CteSpools;
    std::vector<TCteConsumerStateSpec> CteConsumerStates;
    // The graph spec's runtime filter table, which the filter and build
    // shuffle records index.
    std::vector<TRuntimeFilterBindingSpec> RuntimeFilterBindings;
};

// Choose hash-shuffle fanout for a plain grouped aggregate. Explicit shuffle
// settings win; auto mode may use aggregate statistics to exceed the
// conservative join/default fanout.
size_t ChooseAggregatePartitionCount(
    const TAggregateOperator& aggregate,
    size_t inputLanes,
    const TSettings& settings);

// Whether a grouped aggregate should be split into local partial and final
// phases around a repartition exchange. Statistics that predict too little
// reduction rule the partial out; without an estimate the partial phase
// decides at run time.
bool ShouldUsePartialAggregate(
    const TAggregateOperator& aggregate,
    const TSettings& settings);

// Whether an ungrouped or global aggregate over `inputLanes` lanes is lowered
// as a partial->gather->combine cascade instead of one gathered aggregate.
bool ShouldCascadeGlobalAggregate(
    const TAggregateOperator& aggregate,
    size_t inputLanes,
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

// Attach the terminal sink task that writes every output rowset to `sink`; the
// sink must outlive the graph.
void AttachSink(TLoweredPlan& lowered, ISink& sink);

// Attach a remote sink placeholder that takes every output rowset; the node
// that runs the graph binds its code and sends the rows on.
TTaskNode& AttachRemoteSink(TLoweredPlan& lowered);

// The graph spec of a lowered plan with its sink attached. Returns nullopt
// when some nodes have no record; `missing` then lists their indices.
std::optional<TGraphSpec> ExtractLoweredGraphSpec(
    const TLoweredPlan& lowered, std::vector<uint32_t>* missing = nullptr);

// Applies `plan` to the sealed graph of `lowered` (see TTaskGraph::Apply) and
// keeps NodeSpecs in step: the remote sinks and sources get records, and the
// ports Apply moves to new connections move in the records too.
TSealedTaskGraph ApplySplit(TLoweredPlan& lowered, TSplitPlan&& plan);

// The graph spec of each fragment of `lowered`, in Graphs() order: the nodes
// upstream of the fragment's roots. Throws std::runtime_error when some node
// has no record or a fragment cannot stand alone (see SubgraphSpec).
std::vector<TGraphSpec> ExtractFragmentSpecs(const TLoweredPlan& lowered);

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
