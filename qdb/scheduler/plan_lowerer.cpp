#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/scheduler/instrumentation/task.h>
#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/row_join_exec.h>
#include <qdb/exec/late_materialize_exec.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/window_exec.h>
#include <qdb/io/parquet/source.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/union.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/passes/runtime_filters.h>
#include <qdb/plan/ops/cte_consumer.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/executor.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/runtime_adapter.h>
#include <qdb/scheduler/scan_split.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NQdb {
namespace {

struct TSchedulerSourceState {
    ISource* Source = nullptr;
    std::unique_ptr<ISource> OwnedSource;
};

struct TSchedulerUnaryStage {
    std::shared_ptr<const NScheduler::TUnaryCode> Code;
    std::function<std::shared_ptr<void>(size_t)> MakeState;
    NQumir::NAst::TTypePtr OutputType;
};

template <class TState>
struct TProcessorTaskState : NScheduler::ITaskState {
    void ConfigureInstrumentation(TOperatorInstrumentation* instrumentation) override {
        if constexpr (requires(TState& state) { state.Processor.SetInstrumentation(instrumentation); }) {
            static_cast<TState&>(*this).Processor.SetInstrumentation(instrumentation);
        }
    }
};

struct TAggregateBlockingState : TProcessorTaskState<TAggregateBlockingState> {
    explicit TAggregateBlockingState(
        TAggregateKernels kernels,
        TAggregateTablePlan plan)
        : Processor(std::move(kernels), plan)
    {}

    TAggregateProcessor Processor;
    bool Done = false;
};

struct TGroupingSetsBlockingState : TProcessorTaskState<TGroupingSetsBlockingState> {
    TGroupingSetsBlockingState(
        TAggregateKernels kernels, TAggregateTablePlan plan,
        std::vector<std::vector<size_t>> sets, size_t numKeys)
        : Processor(std::move(kernels), plan, std::move(sets), numKeys)
    {}

    TGroupingSetsAggregateProcessor Processor;
    bool Done = false;
};

struct TLimitBlockingState : TProcessorTaskState<TLimitBlockingState> {
    TLimitBlockingState(int64_t limit, int64_t offset)
        : Processor(limit, offset)
    {}

    TLimitProcessor Processor;
};

struct TSortBlockingState : TProcessorTaskState<TSortBlockingState> {
    TSortBlockingState(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel)
        : Processor(
            std::move(outputType),
            std::move(keys),
            std::move(keyColumns),
            std::move(radixKernel))
    {}

    TSortProcessor Processor;
    bool InputFinished = false;
};

struct TWindowBlockingState : TProcessorTaskState<TWindowBlockingState> {
    TWindowBlockingState(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel)
        : Processor(
            std::move(outputType),
            std::move(keys),
            std::move(keyColumns),
            std::move(radixKernel))
    {}

    TWindowProcessor Processor;
    bool InputFinished = false;
};

struct TMergeState : TProcessorTaskState<TMergeState> {
    TMergeState(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        size_t runCount)
        : Processor(
            std::move(outputType),
            std::move(keys),
            std::move(keyColumns),
            runCount)
        , RunFinished(runCount, false)
    {}

    TMergeProcessor Processor;
    std::vector<bool> RunFinished;
    bool InputsDone = false;
};

struct TTopSortBlockingState : TProcessorTaskState<TTopSortBlockingState> {
    TTopSortBlockingState(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel,
        int64_t limit)
        : Processor(
            std::move(outputType),
            std::move(keys),
            std::move(keyColumns),
            std::move(radixKernel),
            limit)
    {}

    TTopSortProcessor Processor;
    bool InputFinished = false;
};

struct TLateMaterializeBlockingState : TProcessorTaskState<TLateMaterializeBlockingState> {
    TLateMaterializeBlockingState(
        std::shared_ptr<const IPhysicalRowReader> reader,
        int32_t locatorColumn)
        : Processor(std::move(reader), locatorColumn)
    {}

    TLateMaterializeProcessor Processor;
    bool InputFinished = false;
};

struct TLateMaterializeMergeState : NScheduler::ITaskState {
    explicit TLateMaterializeMergeState(size_t partitionCount)
        : Partitions(partitionCount)
        , Received(partitionCount, false)
        , Finished(partitionCount, false)
    {}

    ~TLateMaterializeMergeState() {
        for (size_t i = 0; i < Partitions.size(); ++i) {
            if (Received[i]) {
                Release(&Partitions[i]);
            }
        }
    }

    std::vector<TRowSet> Partitions;
    std::vector<bool> Received;
    std::vector<bool> Finished;
    bool Emitted = false;
};

struct TSchedulerInnerJoinState : TProcessorTaskState<TSchedulerInnerJoinState> {
    TSchedulerInnerJoinState(
        TJoinKernels kernels,
        EJoinType joinType,
        EJoinBuildSide buildSide = EJoinBuildSide::Auto)
        : Processor(std::move(kernels), joinType, buildSide)
    {}

    TInnerJoinProcessor Processor;
};

struct TSchedulerRowJoinState : TProcessorTaskState<TSchedulerRowJoinState> {
    TSchedulerRowJoinState(TRowJoinKernels kernels, EJoinBuildSide buildSide, size_t probeHoldBytes)
        : Processor(std::move(kernels), buildSide, probeHoldBytes)
    {}

    TRowJoinProcessor Processor;
};

struct TSchedulerCrossJoinState : TProcessorTaskState<TSchedulerCrossJoinState> {
    TSchedulerCrossJoinState(TCrossJoinKernels kernels, EJoinType joinType)
        : Processor(std::move(kernels), joinType)
    {}

    TCrossJoinProcessor Processor;
};

class TStageDiagnosticsScope {
public:
    TStageDiagnosticsScope(std::ostream* out, const std::string& stage)
        : Out_(out)
    {
        if (Out_) {
            *Out_ << "\n========== RUNTIME STAGE: " << stage << " ==========\n";
        }
    }

    ~TStageDiagnosticsScope() {
        if (Out_) {
            *Out_ << "========== END RUNTIME STAGE ==========\n";
        }
    }

private:
    std::ostream* Out_;
};

TSchedulerUnaryStage BuildSchedulerFilterStage(
    TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options)
{
    auto runtime = BuildFilterRuntimeProcess(
        filter, inputType, std::move(options));
    auto code = std::make_shared<NScheduler::TUnaryCode>(
        [process = std::move(runtime.Process)](void* state, TRowSet& rowSet) {
            auto* kernelState = static_cast<TUnaryStreamingKernelState*>(state);
            process(rowSet, *kernelState);
        });
    return {
        .Code = std::move(code),
        .MakeState = [](size_t) {
            return std::make_shared<TUnaryStreamingKernelState>();
        },
        .OutputType = std::move(runtime.OutputType),
    };
}

TSchedulerUnaryStage BuildSchedulerProjectStage(
    TProjectOperator& project,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options)
{
    auto runtime = BuildProjectRuntimeProcess(
        project, inputType, std::move(options));
    auto code = std::make_shared<NScheduler::TUnaryCode>(
        [process = std::move(runtime.Process)](void* state, TRowSet& rowSet) {
            auto* kernelState = static_cast<TUnaryStreamingKernelState*>(state);
            process(rowSet, *kernelState);
        });

    return {
        .Code = std::move(code),
        .MakeState = [](size_t) {
            return std::make_shared<TUnaryStreamingKernelState>();
        },
        .OutputType = std::move(runtime.OutputType),
    };
}

EJoinFetchResult MapJoinFetch(NScheduler::EFetchResult result)
{
    switch (result) {
        case NScheduler::EFetchResult::OK:
            return EJoinFetchResult::OK;
        case NScheduler::EFetchResult::NO_DATA:
            return EJoinFetchResult::NO_DATA;
        case NScheduler::EFetchResult::FINISHED:
            return EJoinFetchResult::FINISHED;
    }
    return EJoinFetchResult::FINISHED;
}

NScheduler::ETaskResult MapJoinProcessResult(EJoinProcessorResult result)
{
    switch (result) {
        case EJoinProcessorResult::OK:
            return NScheduler::ETaskResult::OK;
        case EJoinProcessorResult::NEED_DATA:
            return NScheduler::ETaskResult::NEED_DATA;
        case EJoinProcessorResult::FINISHED:
            return NScheduler::ETaskResult::FINISHED;
    }
    return NScheduler::ETaskResult::FINISHED;
}

struct TLoweredOutput {
    std::vector<NScheduler::TTaskNode*> Producers;
    NQumir::NAst::TTypePtr OutputType;
};

std::unordered_set<const TSourceOperator*> SourcesFeedingJoins(
    const TOperatorPtr& root)
{
    std::unordered_set<const TSourceOperator*> sources;
    // A shared subtree can be reached both with and without a downstream join.
    std::unordered_map<const IOperator*, unsigned> visited;
    std::function<void(const TOperatorPtr&, bool)> visit =
        [&](const TOperatorPtr& op, bool feedsJoin) {
            if (!op) {
                return;
            }
            feedsJoin = feedsJoin || static_cast<bool>(TMaybeOp<TJoinOperator>(op));
            const unsigned state = feedsJoin ? 2 : 1;
            auto& seen = visited[op.get()];
            if (seen & state) {
                return;
            }
            seen |= state;
            if (auto source = TMaybeOp<TSourceOperator>(op); source && feedsJoin) {
                sources.insert(source.Cast().get());
            }
            if (auto consumer = TMaybeOp<TCteConsumer>(op)) {
                visit(consumer.Cast()->Materialization()->Plan, feedsJoin);
            }
            for (const auto& input : op->Inputs()) {
                visit(input, feedsJoin);
            }
        };
    visit(root, false);
    return sources;
}

size_t ChooseRuntimeBloomBytes(const TJoinOperator& join,
    EJoinFilterSide buildSide, size_t producerCount)
{
    constexpr size_t totalBudget = 128u << 20;
    const size_t maximum = std::bit_floor(std::max<size_t>(
        64, totalBudget / (producerCount + 1)));
    const size_t minimum = std::min(
        TRuntimeFilter::DefaultBloomBytes, maximum);
    const auto& build = buildSide == EJoinFilterSide::Left
        ? join.Left() : join.Right();
    if (!build->Stats_) {
        return minimum;
    }
    // The builder needs a fixed size before its lanes begin. Reserve up to
    // twelve bits per estimated build row, bounded across all lane-local
    // fragments and the merged filter. A saturated filter is disabled later.
    const auto estimatedBytes = static_cast<size_t>(std::min<long double>(
        static_cast<long double>(maximum),
        static_cast<long double>(build->Stats_->RowCount) * 1.5L));
    return std::bit_ceil(std::clamp(estimatedBytes, minimum, maximum));
}

bool CanReuseRuntimeFilterHash(const TOperatorPtr& probe, uint32_t filterId) {
    // Lower() applies annotations after the operator, in vector order. The
    // final consumer owns the outgoing hash even when the operator is a join.
    if (!probe->RuntimeFilterUses.empty()) {
        return probe->RuntimeFilterUses.back().Id == filterId;
    }
    if (TMaybeOp<TJoinOperator>(probe)) {
        return false;
    }
    for (const auto& child : probe->Inputs()) {
        if (!CanReuseRuntimeFilterHash(child, filterId)) {
            return false;
        }
    }
    return true;
}

class TSchedulerGraphLowerer {
public:
    struct TFilterConsumerTasks {
        std::shared_ptr<NScheduler::TRuntimeFilterCode> Code;
        std::vector<NScheduler::TTaskNode*> Nodes;
        std::vector<std::string> ProbeKeys;
        NQumir::NAst::TTypePtr OutputType;
        bool AtSource = false;
    };

    struct TFilterProducerTasks {
        uint32_t Id;
        NKernel::TOperatorKernelSpec KernelSpec;
        TJoinHashKernels HashKernels;
        std::shared_ptr<IRuntimeFilterProbe> Probe;
        std::vector<NScheduler::TTaskNode*> Nodes;
        EJoinFilterSide BuildSide;
        TExecStageId ExecStageId;
        std::string Group;
    };

    TSchedulerGraphLowerer(
        NScheduler::TTaskGraph& graph,
        const TOperatorPtr& root,
        NScheduler::TSettings settings,
        std::ostream* diagnostics,
        std::vector<TGeneratedKernel>* kernelSink,
        std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog,
        std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings)
        : Graph_(graph)
        , Settings_(std::move(settings))
        , Diagnostics_(diagnostics)
        , KernelSink_(kernelSink)
        , ExternalCatalog_(std::move(externalCatalog))
        , RuntimeFilterBindings_(std::move(filterBindings))
        , RuntimeContext_(std::make_shared<TRuntimeContext>())
        , SourcesFeedingJoins_(Settings_.ScanSplit.MaxScanTasks == 0
            ? SourcesFeedingJoins(root)
            : std::unordered_set<const TSourceOperator*>{})
    {}

    // Kernel options for one stage: generation only (deferred finalization),
    // kernels collected into the lowered plan.
    TKernelCompilerOptions KernelOptions(
        std::string stage,
        TExecStageId execStageId) const
    {
        return TKernelCompilerOptions{
            .Diagnostics = Diagnostics_,
            .Stage = std::move(stage),
            .ExecStageId = execStageId,
            .Sink = KernelSink_,
            .BindNow = false,
            .ExternalCatalog = ExternalCatalog_,
            .RuntimeContext = RuntimeContext_,
        };
    }

    size_t OutputLanes(const TOperatorPtr& op) const {
        if (auto n = TMaybeOp<TSourceOperator>(op)) {
            auto splits = ScanSplits(*n.Cast());
            return std::max<size_t>(splits ? splits->size() : 0, 1);
        }
        if (TMaybeOp<TCteConsumer>(op)) {
            return 1;
        }
        if (auto n = TMaybeOp<TFilterOperator>(op)) {
            return OutputLanes(n.Cast()->Input());
        }
        if (auto n = TMaybeOp<TProjectOperator>(op)) {
            return OutputLanes(n.Cast()->Input());
        }
        if (auto n = TMaybeOp<TAggregateOperator>(op)) {
            const size_t childLanes = OutputLanes(n.Cast()->Input());
            if (childLanes == 0) {
                return 0;
            }
            // Grouped aggregate over a parallel input is hash-shuffled by group
            // key into partition-local aggregates (disjoint groups), so its
            // output keeps the shuffle partition count. Global aggregates
            // (`__group__` constant) stay single. Grouping sets shuffle their
            // partials by (__grouping_id__ + keys), so they too keep the count.
            if (!n.Cast()->IsGlobal() &&
                !n.Cast()->GroupKeys().empty() && childLanes > 1)
            {
                return n.Cast()->GroupingSets().empty()
                    ? AggregatePartitions(*n.Cast(), childLanes)
                    : JoinPartitions(childLanes);
            }
            return 1;
        }
        if (auto n = TMaybeOp<TWindowOperator>(op)) {
            const size_t childLanes = OutputLanes(n.Cast()->Input());
            if (childLanes == 0) {
                return 0;
            }
            if (!n.Cast()->PartitionKeys().empty() && childLanes > 1) {
                return JoinPartitions(childLanes);
            }
            return 1;
        }
        if (TMaybeOp<TLimitOperator>(op) ||
            TMaybeOp<TSortOperator>(op) ||
            TMaybeOp<TTopSortOperator>(op) ||
            TMaybeOp<TLateMaterializeOperator>(op))
        {
            return SupportedChild(op) ? 1 : 0;
        }
        if (auto n = TMaybeOp<TJoinOperator>(op)) {
            auto join = n.Cast();
            if (join->Keys().empty()) {
                return KeylessJoinLanes(*join);
            }
            // Inner, left/right/full outer, and left semi/anti equi-joins are
            // hash-partitionable (matches co-locate by key).
            const bool supportedType =
                join->JoinType() == EJoinType::Inner ||
                join->JoinType() == EJoinType::Left ||
                join->JoinType() == EJoinType::Right ||
                join->JoinType() == EJoinType::Full ||
                join->JoinType() == EJoinType::LeftSemi ||
                join->JoinType() == EJoinType::LeftAnti;
            if (!supportedType) {
                return 0;
            }
            const size_t leftLanes = OutputLanes(join->Left());
            const size_t rightLanes = OutputLanes(join->Right());
            if (leftLanes == 0 || rightLanes == 0) {
                return 0;
            }
            return JoinPartitions(std::max(leftLanes, rightLanes));
        }
        if (auto n = TMaybeOp<TUnionAllOperator>(op)) {
            size_t total = 0;
            for (const auto& branch : n.Cast()->Inputs()) {
                size_t branchLanes = OutputLanes(branch);
                if (branchLanes == 0) {
                    return 0;
                }
                total += branchLanes;
            }
            return total;
        }
        return 0;
    }

    // A keyless (nested-loop) join broadcasts its right side to every lane of
    // the left side, which keeps its partition count when each left row's
    // output depends only on that row: inner, left outer, and left semi/anti.
    // RIGHT / FULL pad the right rows no left row matched, which only the
    // whole left input decides, so they run on one lane.
    size_t KeylessJoinLanes(TJoinOperator& join) const {
        const size_t lanes = OutputLanes(join.Left());
        const size_t rightLanes = OutputLanes(join.Right());
        if (lanes == 0 || rightLanes == 0) {
            return 0;
        }
        switch (join.JoinType()) {
            case EJoinType::Inner:
            case EJoinType::Left:
            case EJoinType::LeftSemi:
            case EJoinType::LeftAnti:
                return lanes;
            case EJoinType::Right:
            case EJoinType::Full:
                return 1;
            case EJoinType::RightSemi:
            case EJoinType::RightAnti:
            case EJoinType::LeftMark:
                return 0;
        }
        return 0;
    }

    // outLaneOffset shifts this operator's producer lanes within outConn, so a
    // UNION ALL can fan several branches into one gather without lane collisions.
    TLoweredOutput Lower(
        const TOperatorPtr& op,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset = 0)
    {
        if (op->RuntimeFilterUses.empty()) {
            return LowerRaw(op, outConn, outLaneOffset);
        }
        const size_t lanes = OutputLanes(op);
        auto& firstInput = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "runtime-filter-input");
        auto lowered = LowerRaw(op, firstInput, 0);
        NScheduler::IConnection* input = &firstInput;
        for (size_t index = 0; index < op->RuntimeFilterUses.size(); ++index) {
            const auto& use = op->RuntimeFilterUses[index];
            NScheduler::IConnection* output = &outConn;
            if (index + 1 != op->RuntimeFilterUses.size()) {
                output = &AddConn<NScheduler::TOneToOneConnection>(
                    lanes, lanes, "runtime-filter-chain");
            }
            auto code = std::make_shared<NScheduler::TRuntimeFilterCode>();
            code->AuxInput = std::make_shared<NScheduler::TAuxInput>();
            const auto execStageId = NewExecStage(
                op.get(), EExecPlanNodeKind::RuntimeFilter);
            ExecStages_.back().RuntimeFilterUse = use;
            TFilterConsumerTasks consumer{
                .Code = code,
                .ProbeKeys = use.ProbeKeys,
                .OutputType = lowered.OutputType,
                .AtSource = static_cast<bool>(TMaybeOp<TSourceOperator>(op)),
            };
            consumer.Nodes.reserve(lanes);
            const auto group = NewTaskGroup();
            for (size_t lane = 0; lane < lanes; ++lane) {
                auto task = std::make_unique<NScheduler::TRuntimeFilterTask>(
                    code,
                    NScheduler::TInputPort{.Connection = input, .Lane = lane},
                    NScheduler::TOutputPort{
                        .Connection = output,
                        .Lane = lane + (output == &outConn ? outLaneOffset : 0)});
                auto& node = Graph_.AddOwnedNode(std::move(task));
                MarkNode(node, "runtime-filter", group,
                    "runtime-filter " + std::to_string(use.Id), execStageId);
                Graph_.AddEdge(*lowered.Producers[lane], node, *input, lane, lane);
                consumer.Nodes.push_back(&node);
            }
            lowered.Producers = consumer.Nodes;
            FilterConsumers_[use.Id].push_back(std::move(consumer));
            input = output;
        }
        return lowered;
    }

    TLoweredOutput LowerRaw(
        const TOperatorPtr& op,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        if (auto n = TMaybeOp<TSourceOperator>(op)) {
            return LowerSource(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TCteConsumer>(op)) {
            return LowerCteConsumer(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TFilterOperator>(op)) {
            auto filter = n.Cast();
            auto execStageId = NewExecStage(
                filter.get(), EExecPlanNodeKind::Filter);
            auto stageLabel = StageLabel("filter", execStageId);
            return LowerUnary(
                filter->Input(),
                "filter",
                stageLabel,
                execStageId,
                [&](const NQumir::NAst::TTypePtr& inType) {
                    return BuildSchedulerFilterStage(
                        *filter,
                        inType,
                        KernelOptions(stageLabel, execStageId));
                },
                outConn,
                outLaneOffset);
        }
        if (auto n = TMaybeOp<TProjectOperator>(op)) {
            auto project = n.Cast();
            auto execStageId = NewExecStage(
                project.get(), EExecPlanNodeKind::Project);
            auto stageLabel = StageLabel("project", execStageId);
            return LowerUnary(
                project->Input(),
                "project",
                stageLabel,
                execStageId,
                [&](const NQumir::NAst::TTypePtr& inType) {
                    return BuildSchedulerProjectStage(
                        *project,
                        inType,
                        KernelOptions(stageLabel, execStageId));
                },
                outConn,
                outLaneOffset);
        }
        if (auto n = TMaybeOp<TAggregateOperator>(op)) {
            return LowerAggregate(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TWindowOperator>(op)) {
            return LowerWindow(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TLimitOperator>(op)) {
            return LowerLimit(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TSortOperator>(op)) {
            return LowerSort(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TTopSortOperator>(op)) {
            return LowerTopSort(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TLateMaterializeOperator>(op)) {
            return LowerLateMaterialize(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TJoinOperator>(op)) {
            // The mark join kernel exists, but no join processor drives it yet.
            if (n.Cast()->JoinType() == EJoinType::LeftMark) {
                throw std::runtime_error("left_mark join is not supported by execution yet");
            }
            if (n.Cast()->Keys().empty()) {
                return LowerCrossJoin(*n.Cast(), outConn, outLaneOffset);
            }
            return LowerJoin(*n.Cast(), outConn, outLaneOffset);
        }
        if (auto n = TMaybeOp<TUnionAllOperator>(op)) {
            return LowerUnionAll(*n.Cast(), outConn, outLaneOffset);
        }
        throw std::runtime_error("scheduler lowering: unsupported operator");
    }

    // UNION ALL is a no-op at runtime: lower every branch straight into outConn,
    // giving each branch a contiguous lane range so the gather sees all rows.
    TLoweredOutput LowerUnionAll(
        TUnionAllOperator& un,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        TLoweredOutput result;
        size_t offset = outLaneOffset;
        for (const auto& branch : un.Inputs()) {
            auto branchOut = Lower(branch, outConn, offset);
            if (result.Producers.empty()) {
                result.OutputType = branchOut.OutputType;
            }
            for (auto* producer : branchOut.Producers) {
                result.Producers.push_back(producer);
            }
            offset += branchOut.Producers.size();
        }
        return result;
    }

    size_t QueueCapacity() const {
        return std::max<size_t>(Settings_.Queue.RowsetCapacityPerLane, 1);
    }

    void CheckMaterializationsWired() const {
        for (const auto& entry : Materialized_) {
            if (entry.second.NextConsumerLane != entry.first->RefCount) {
                throw std::runtime_error(
                    "cte materialization: completion fan-out does not match RefCount");
            }
        }
    }

    void WireRuntimeFilters() {
        std::unordered_set<uint32_t> seenIds;
        for (const auto& producer : FilterProducers_) {
            if (!seenIds.insert(producer.Id).second) {
                throw std::runtime_error(
                    "runtime filter id is produced by more than one join");
            }
            auto found = FilterConsumers_.find(producer.Id);
            if (found == FilterConsumers_.end() || found->second.empty()) {
                throw std::runtime_error(
                    "runtime filter has no planned consumer");
            }
            auto& consumers = found->second;
            const bool buildIsLeft =
                producer.BuildSide == EJoinFilterSide::Left;
            const auto& spec = producer.KernelSpec;
            auto probeSchema = NQumir::NAst::TMaybeType<
                NQumir::NAst::TStructType>(
                    spec.InputSchemas[buildIsLeft ? 1 : 0]);
            if (!probeSchema) {
                throw std::runtime_error(
                    "runtime filter producer has invalid probe schema");
            }
            TKernelCompiler compiler(
                KernelOptions(producer.Group, producer.ExecStageId));
            for (auto& consumer : consumers) {
                auto consumerSchema = NQumir::NAst::TMaybeType<
                    NQumir::NAst::TStructType>(consumer.OutputType);
                if (!consumerSchema
                    || consumer.ProbeKeys.size() != spec.JoinKeys.size()) {
                    throw std::runtime_error(
                        "runtime filter consumer has invalid probe schema or keys");
                }
                auto consumerHashSpec = spec;
                consumerHashSpec.InputSchemas[buildIsLeft ? 1 : 0] =
                    consumer.OutputType;
                for (size_t i = 0; i < spec.JoinKeys.size(); ++i) {
                    const auto& originalKey = buildIsLeft
                        ? spec.JoinKeys[i].Right.Name
                        : spec.JoinKeys[i].Left.Name;
                    const auto originalType = FieldType(
                        probeSchema.Cast().get(), originalKey);
                    const auto consumerType = FieldType(
                        consumerSchema.Cast().get(), consumer.ProbeKeys[i]);
                    const auto& fields = consumerSchema.Cast()->Fields;
                    const auto field = std::find_if(fields.begin(), fields.end(),
                        [&](const auto& item) {
                            return item.first == consumer.ProbeKeys[i];
                        });
                    if (!originalType || !consumerType
                        || field == fields.end()
                        || originalType->ToString()
                            != consumerType->ToString()) {
                        throw std::runtime_error(
                            "runtime filter consumer key type differs from join probe key");
                    }
                    auto& ref = buildIsLeft
                        ? consumerHashSpec.JoinKeys[i].Right
                        : consumerHashSpec.JoinKeys[i].Left;
                    ref.Name = consumer.ProbeKeys[i];
                    ref.Index = static_cast<int32_t>(
                        std::distance(fields.begin(), field));
                    ref.Type = consumerType;
                }
                if (consumer.OutputType->ToString()
                        == spec.InputSchemas[buildIsLeft ? 1 : 0]->ToString()
                    && std::equal(consumer.ProbeKeys.begin(),
                        consumer.ProbeKeys.end(), spec.JoinKeys.begin(),
                        [buildIsLeft](const auto& name, const auto& key) {
                            return name == (buildIsLeft
                                ? key.Right.Name : key.Left.Name);
                        })) {
                    consumer.Code->Hash = buildIsLeft
                        ? producer.HashKernels.Right
                        : producer.HashKernels.Left;
                } else {
                    auto consumerHash = compiler.CompileJoinHash(
                        consumerHashSpec);
                    consumer.Code->Hash = buildIsLeft
                        ? std::move(consumerHash.Right)
                        : std::move(consumerHash.Left);
                }
                consumer.Code->Probe = producer.Probe;
                consumer.Code->AuxInput->AddFilter(producer.Probe);
                for (auto* buildNode : producer.Nodes) {
                    consumer.Code->AuxInput->AddProducer(*buildNode);
                    for (auto* probeNode : consumer.Nodes) {
                        Graph_.AddDependencyEdge(*buildNode, *probeNode);
                    }
                }
            }
            RuntimeFilters_.push_back({
                .Id = producer.Id,
                .Probe = producer.Probe,
                .AtSource = std::all_of(consumers.begin(), consumers.end(),
                    [](const auto& consumer) { return consumer.AtSource; }),
            });
        }
    }

    void CheckExecStagesConsistent() const {
        for (const auto& [id, consumers] : FilterConsumers_) {
            for (const auto& consumer : consumers) {
                if (!consumer.Code->Probe || !consumer.Code->Hash) {
                    throw std::runtime_error(
                        "runtime filter consumer " + std::to_string(id)
                        + " has no producer binding");
                }
            }
        }
        std::vector<size_t> taskCounts(ExecStages_.size() + 1, 0);
        for (const auto& node : Graph_.Nodes()) {
            if (node->TaskGroupId == NScheduler::InvalidTaskGroupId) {
                throw std::runtime_error(
                    "scheduler lowering: task has no stable task group");
            }
            const auto id = node->ExecStageId;
            if (id == InvalidExecStageId) {
                continue; // scheduler-only plumbing
            }
            if (id >= taskCounts.size()) {
                throw std::runtime_error(
                    "scheduler lowering: task references an unknown exec stage");
            }
            ++taskCounts[id];
        }
        for (const auto& stage : ExecStages_) {
            if (stage.Id == InvalidExecStageId ||
                stage.Id >= taskCounts.size() || taskCounts[stage.Id] == 0)
            {
                throw std::runtime_error(
                    "scheduler lowering: exec stage has no physical tasks");
            }
        }
        if (KernelSink_) {
            for (const auto& kernel : *KernelSink_) {
                if (kernel.ExecStageId == InvalidExecStageId ||
                    kernel.ExecStageId >= taskCounts.size())
                {
                    throw std::runtime_error(
                        "scheduler lowering: kernel references an unknown exec stage");
                }
            }
        }
    }

private:
    static std::string StageLabel(
        std::string_view kind,
        TExecStageId execStageId)
    {
        return "exec:" + std::to_string(execStageId) + ":" +
            std::string(kind);
    }

    static void MarkNode(
        NScheduler::TTaskNode& node,
        std::string kind,
        NScheduler::TTaskGroupId taskGroupId,
        std::string label,
        TExecStageId execStageId = InvalidExecStageId)
    {
        node.ExecStageId = execStageId;
        node.TaskGroupId = taskGroupId;
        node.DebugKind = std::move(kind);
        node.DebugLabel = std::move(label);
    }

    static std::string JoinDebugLabel(const TJoinOperator& join) {
        if (join.Keys().empty()) {
            return "cross-join " + std::string(JoinTypeName(join.JoinType()));
        }
        std::string label = "join " + std::string(JoinTypeName(join.JoinType()));
        const auto& keys = join.Keys();
        for (size_t i = 0; i < keys.size(); ++i) {
            label += (i ? ", " : " [") + keys[i].Left
                + (keys[i].NullsEqual ? " IS NOT DISTINCT FROM " : " = ") + keys[i].Right;
        }
        label += "]";
        if (join.Filter()) {
            label += " residual";
        }
        return label;
    }

    struct TBlockingTail {
        std::shared_ptr<const NScheduler::TBlockingCode> Code;
        std::function<std::shared_ptr<NScheduler::ITaskState>()> MakeState;
        NQumir::NAst::TTypePtr OutputType;
        // Appended to the label of the tasks, e.g. an aggregate's table.
        std::string LabelDetail;
    };

    // The table an aggregate task starts with and its switch to rows.
    static std::string AggregateTableLabel(const TAggregateTablePlan& plan) {
        if (plan.Layout == EAggregateTableLayout::Row) {
            return " [row table]";
        }
        if (plan.RowSwitchGroups == 0) {
            return " [swiss table]";
        }
        return " [swiss table, rows from " + std::to_string(plan.RowSwitchGroups) + " groups]";
    }

    struct TRepartitionExchange {
        NScheduler::THashShuffleConnection* Connection = nullptr;
        std::vector<NScheduler::TTaskNode*> Nodes;
    };

    // Blocking code for a processor with Add/Next/InputFinished methods. It
    // drains the input first, then emits processor output.
    template <class TState>
    std::shared_ptr<const NScheduler::TBlockingCode> MakeDrainBlockingCode() {
        auto code = std::make_shared<NScheduler::TBlockingCode>(
            [](NScheduler::ITaskState& state, NScheduler::TInputPort& input, TRowSet& output) {
                auto* s = &static_cast<TState&>(state);
                TRowSet rowSet{};
                while (!s->InputFinished) {
                    auto fetch = input.Fetch(rowSet);
                    if (fetch == NScheduler::EFetchResult::NO_DATA) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    if (fetch == NScheduler::EFetchResult::FINISHED) {
                        s->InputFinished = true;
                        break;
                    }
                    s->Processor.Add(rowSet);
                    rowSet = {};
                }
                if (s->Processor.Next(output)) {
                    return NScheduler::ETaskResult::OK;
                }
                return NScheduler::ETaskResult::FINISHED;
            });
        return code;
    }

    // Blocking tail for a local sort (top-K when topLimit is set, otherwise a
    // full sort). Shared by the single-lane path and the per-lane locals of the
    // partitioned merge.
    TBlockingTail MakeSortTail(
        const NQumir::NAst::TTypePtr& childType,
        const std::vector<TSortKey>& keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel radixKernel,
        std::optional<int64_t> topLimit)
    {
        if (topLimit) {
            const int64_t limit = *topLimit;
            return TBlockingTail{
                .Code = MakeDrainBlockingCode<TTopSortBlockingState>(),
                .MakeState = [childType, keys, keyColumns, radixKernel, limit]()
                    -> std::shared_ptr<NScheduler::ITaskState>
                {
                    return std::make_shared<TTopSortBlockingState>(
                        childType, keys, keyColumns, radixKernel, limit);
                },
                .OutputType = childType,
            };
        }
        return TBlockingTail{
            .Code = MakeDrainBlockingCode<TSortBlockingState>(),
            .MakeState = [childType, keys, keyColumns, radixKernel]()
                -> std::shared_ptr<NScheduler::ITaskState>
            {
                return std::make_shared<TSortBlockingState>(
                    childType, keys, keyColumns, radixKernel);
            },
            .OutputType = childType,
        };
    }

    // Blocking code for a limit/offset task.
    std::shared_ptr<const NScheduler::TBlockingCode> MakeLimitBlockingCode() {
        return std::make_shared<NScheduler::TBlockingCode>(
            [](NScheduler::ITaskState& state, NScheduler::TInputPort& input, TRowSet& output) {
                auto* s = &static_cast<TLimitBlockingState&>(state);
                if (s->Processor.Finished()) {
                    return NScheduler::ETaskResult::FINISHED;
                }
                TRowSet rowSet{};
                while (!s->Processor.Finished()) {
                    auto fetch = input.Fetch(rowSet);
                    if (fetch == NScheduler::EFetchResult::NO_DATA) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    if (fetch == NScheduler::EFetchResult::FINISHED) {
                        return NScheduler::ETaskResult::FINISHED;
                    }
                    if (s->Processor.Process(rowSet, output)) {
                        return NScheduler::ETaskResult::OK;
                    }
                    rowSet = {};
                }
                return NScheduler::ETaskResult::FINISHED;
            });
    }

    // Binary blocking code adapting scheduler input ports to a join/cross
    // processor's fetch-callback interface. TState wraps the processor.
    template <class TState>
    std::shared_ptr<const NScheduler::TBinaryBlockingCode> MakeBinaryJoinCode() {
        auto code = std::make_shared<NScheduler::TBinaryBlockingCode>(
            [](NScheduler::ITaskState& state,
               NScheduler::TInputPort& left,
               NScheduler::TInputPort& right,
               TRowSet& output)
            {
                auto* s = &static_cast<TState&>(state);
                auto fetchLeft = [&](TRowSet& rowSet) {
                    return MapJoinFetch(left.Fetch(rowSet));
                };
                auto fetchRight = [&](TRowSet& rowSet) {
                    return MapJoinFetch(right.Fetch(rowSet));
                };
                const auto result = s->Processor.Process(fetchLeft, fetchRight, output);
                if constexpr (requires { s->Processor.ObserveInstrumentation(); }) {
                    s->Processor.ObserveInstrumentation();
                }
                return MapJoinProcessResult(result);
            });
        if constexpr (requires(TState& s) { s.Processor.RequiredInputSide(); }) {
            code->RequiredSide = [](NScheduler::ITaskState& state) {
                switch (static_cast<TState&>(state).Processor.RequiredInputSide()) {
                    case EJoinBuildSide::Left:
                        return NScheduler::ERequiredSide::Left;
                    case EJoinBuildSide::Right:
                        return NScheduler::ERequiredSide::Right;
                    case EJoinBuildSide::Auto:
                        return NScheduler::ERequiredSide::Both;
                }
                return NScheduler::ERequiredSide::Both;
            };
        }
        return code;
    }

    bool SupportedChild(const TOperatorPtr& op) const {
        if (auto n = TMaybeOp<TAggregateOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        if (auto n = TMaybeOp<TLimitOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        if (auto n = TMaybeOp<TSortOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        if (auto n = TMaybeOp<TTopSortOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        if (auto n = TMaybeOp<TLateMaterializeOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        if (auto n = TMaybeOp<TWindowOperator>(op)) {
            return OutputLanes(n.Cast()->Input()) != 0;
        }
        return false;
    }

    using TSourceSplits =
        std::optional<std::vector<NScheduler::TScanSplit>>;

    TSourceSplits ScanSplits(TSourceOperator& src) const {
        if (auto found = ScanSplitCache_.find(&src);
            found != ScanSplitCache_.end())
        {
            return found->second;
        }
        auto* metadata =
            dynamic_cast<NScheduler::IScanMetadataSource*>(&src.GetSource());
        if (!metadata) {
            return std::nullopt;
        }
        std::vector<NScheduler::TScanRowGroup> rowGroups;
        if (auto* parquet = dynamic_cast<TParquetSource*>(&src.GetSource())) {
            // OutputLanes runs before BuildSourceRuntimeType restricts the
            // reader. Estimate the logical scan's projection without mutating
            // a source that may also be used by an inlined CTE reference.
            std::optional<std::unordered_set<std::string>> columns;
            if (auto required = src.RequiredColumns();
                required && Settings_.ScanSplit.MaxScanTasks == 0)
            {
                columns.emplace();
                for (const auto& [name, _] : required->Fields) {
                    const auto dot = name.rfind('.');
                    columns->insert(dot == std::string::npos
                        ? name : name.substr(dot + 1));
                }
            }
            rowGroups = parquet->PruneRowGroups(
                src.RowGroupPredicate(),
                src.GetAlias(),
                Settings_.Instrumentation.RowGroupPredicate
                    ? Settings_.Instrumentation.Output
                    : nullptr,
                columns ? &*columns : nullptr);
        } else {
            rowGroups = metadata->ScanRowGroups();
        }
        if (rowGroups.empty()) {
            TSourceSplits empty = std::vector<NScheduler::TScanSplit>{};
            ScanSplitCache_.emplace(&src, empty);
            return empty;
        }
        auto splitSettings = Settings_.ScanSplit;
        splitSettings.MaxScanTasks = NScheduler::ChooseScanTaskCount(
            rowGroups, Settings_, SourcesFeedingJoins_.contains(&src));
        auto splits = NScheduler::BuildScanSplits(rowGroups, splitSettings);
        if (Diagnostics_ && Settings_.ScanSplit.MaxScanTasks == 0) {
            int64_t rows = 0;
            int64_t bytes = 0;
            for (const auto& group : rowGroups) {
                rows += group.RowCount;
                bytes += group.ByteSize;
            }
            *Diagnostics_ << "scan split: source="
                << (src.GetAlias().empty() ? src.SourcePath() : src.GetAlias())
                << " scan_tasks=auto max_tasks=" << splitSettings.MaxScanTasks
                << " tasks=" << splits.size()
                << " row_groups=" << rowGroups.size()
                << " rows=" << rows << " bytes=" << bytes << '\n';
        }
        // An engaged empty value is reserved for a proven empty scan. If a
        // future strategy declines a non-empty input by returning {}, use the
        // original source rather than silently dropping all rows.
        TSourceSplits result = splits.empty()
            ? TSourceSplits{std::nullopt}
            : TSourceSplits{std::move(splits)};
        ScanSplitCache_.emplace(&src, result);
        return result;
    }

    // The row join table (kernel/join/join_rows.oz) serves a join that
    // RowJoinTableServes accepts once lowering has fixed its build side, by
    // estimates or by a runtime filter. A join without estimates keeps the
    // symmetric Robin Hood join.
    bool UsesRowJoinTable(const TJoinOperator& join, EJoinBuildSide buildSide) const {
        return RowJoinTableServes(join, Settings_.Join.Table, buildSide);
    }

    // Number of hash-shuffle partitions for a shuffled operator (grouped
    // aggregate / equi-join). Defaults to the input's own parallelism
    // (`inputLanes` - its scan-split lane count, a data-size proxy) capped at
    // half the worker count: small inputs stay narrow, large inputs fan out, and
    // the half-worker cap guards against shuffle over-sharding on big inputs
    // (measured to regress a few queries at cap = WorkerCount).
    // `--shuffle-partitions` overrides.
    size_t JoinPartitions(size_t inputLanes) const {
        auto partitions = Settings_.HashShuffle.PartitionCount;
        if (partitions == 0) {
            auto cap = std::max<size_t>(Settings_.Scheduler.WorkerCount / 2, 1);
            partitions = std::min(inputLanes, cap);
        }

        auto maxPartitions = Settings_.HashShuffle.MaxPartitionCount;
        if (maxPartitions == 0) {
            maxPartitions = partitions;
        }

        return std::max<size_t>(
            std::min(partitions, maxPartitions),
            1);
    }

    size_t AggregatePartitions(
        const TAggregateOperator& aggregate,
        size_t inputLanes) const
    {
        return NScheduler::ChooseAggregatePartitionCount(
            aggregate, inputLanes, Settings_);
    }

    size_t ShuffleQueueCapacity() const {
        return std::max<size_t>(
            Settings_.HashShuffle.MaxQueuedRowsetsPerLane,
            1);
    }

    // Create a connection of the given type, size it, name it for diagnostics,
    // register it in the graph, and return a reference to the stored object.
    template <class TConn, class... TArgs>
    TConn& AddConn(
        size_t srcLanes,
        size_t dstLanes,
        std::string name,
        size_t capacity,
        TArgs&&... args)
    {
        auto conn = std::make_unique<TConn>(
            capacity, std::forward<TArgs>(args)...);
        conn->Resize(srcLanes, dstLanes);
        conn->SetDebugName(std::move(name));
        return static_cast<TConn&>(Graph_.AddConnection(std::move(conn)));
    }

    template <class TConn>
    TConn& AddConn(size_t srcLanes, size_t dstLanes, std::string name) {
        return AddConn<TConn>(
            srcLanes, dstLanes, std::move(name), QueueCapacity());
    }

    TLoweredOutput LowerSource(
        TSourceOperator& src,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        auto outputType = BuildSourceRuntimeType(src);
        const auto execStageId = NewExecStage(
            &src, EExecPlanNodeKind::Source);
        auto splitPlan = ScanSplits(src);
        auto* sourcePtr = &src.GetSource();
        auto* parquetSource = dynamic_cast<TParquetSource*>(sourcePtr);
        auto code = std::make_shared<NScheduler::TSourceCode>(
            [](void* state, TRowSet& rowSet) {
                auto* sourceState = static_cast<TSchedulerSourceState*>(state);
                return sourceState->Source->Next(rowSet);
            });

        const size_t lanes = std::max<size_t>(
            splitPlan ? splitPlan->size() : 0, 1);
        TLoweredOutput result;
        result.OutputType = std::move(outputType);
        result.Producers.reserve(lanes);
        for (size_t p = 0; p < lanes; ++p) {
            const auto* split = splitPlan && !splitPlan->empty()
                ? &(*splitPlan)[p]
                : nullptr;
            std::shared_ptr<void> state;
            if (parquetSource && split && !split->RowGroups.empty()) {
                auto owned = parquetSource->MakeRowGroupsSource(split->RowGroups);
                auto* ptr = owned.get();
                state = std::make_shared<TSchedulerSourceState>(
                    TSchedulerSourceState{
                        .Source = ptr,
                        .OwnedSource = std::move(owned),
                    });
            } else if (parquetSource && splitPlan && splitPlan->empty()) {
                auto owned = parquetSource->MakeRowGroupsSource({});
                auto* ptr = owned.get();
                state = std::make_shared<TSchedulerSourceState>(
                    TSchedulerSourceState{
                        .Source = ptr,
                        .OwnedSource = std::move(owned),
                    });
            } else {
                state = std::make_shared<TSchedulerSourceState>(
                    TSchedulerSourceState{.Source = sourcePtr});
            }
            auto task = std::make_unique<NScheduler::TSourceTask>(
                code,
                std::move(state),
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = p + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "source",
                execStageId,
                src.GetAlias().empty() ? "source" : "source " + src.GetAlias(),
                execStageId);
            result.Producers.push_back(&node);
        }
        return result;
    }

    struct TMaterializedProducer {
        std::shared_ptr<NScheduler::TCteSharedState> State;
        NScheduler::TTaskNode* Producer = nullptr;
        NScheduler::IConnection* Completion = nullptr;
        size_t NextConsumerLane = 0;
        NQumir::NAst::TTypePtr OutputType;
    };

    TLoweredOutput LowerCteConsumer(
        TCteConsumer& consumer,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const auto* mat = consumer.Materialization().get();
        auto it = Materialized_.find(mat);
        if (it == Materialized_.end()) {
            it = Materialized_.emplace(
                mat, BuildMaterializedProducer(consumer)).first;
        }
        auto& producer = it->second;

        const size_t lane = producer.NextConsumerLane++;
        if (lane >= consumer.Materialization()->RefCount) {
            throw std::runtime_error(
                "cte materialization: more consumers lowered than RefCount");
        }
        auto task = std::make_unique<NScheduler::TCteConsumerTask>(
            producer.State,
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = outLaneOffset});
        auto& node = Graph_.AddOwnedNode(std::move(task));
        const auto execStageId = NewExecStage(
            &consumer, EExecPlanNodeKind::CteConsumer);
        MarkNode(node,
            "cte-consumer",
            execStageId,
            "cte-consumer " + std::to_string(consumer.Def()->Id),
            execStageId);
        Graph_.AddEdge(*producer.Producer, node, *producer.Completion, 0, lane);
        return TLoweredOutput{
            .Producers = {&node},
            .OutputType = producer.OutputType,
        };
    }

    TMaterializedProducer BuildMaterializedProducer(TCteConsumer& consumer) {
        const auto& materialization = consumer.Materialization();
        const size_t lanes = OutputLanes(materialization->Plan);
        NScheduler::IConnection* inputConn = nullptr;
        if (lanes == 1) {
            inputConn = &AddConn<NScheduler::TOneToOneConnection>(
                1, 1, "cte-producer-input");
        } else {
            inputConn = &AddConn<NScheduler::TGatherConnection>(
                lanes, 1, "cte-producer-gather");
        }
        auto planOut = Lower(materialization->Plan, *inputConn);

        auto state = std::make_shared<NScheduler::TCteSharedState>();
        auto code = std::make_shared<NScheduler::TBlockingCode>(
            [](NScheduler::ITaskState& st, NScheduler::TInputPort& input, TRowSet&) {
                auto* shared = &static_cast<NScheduler::TCteSharedState&>(st);
                for (;;) {
                    TRowSet batch{};
                    auto fetch = input.Fetch(batch);
                    if (fetch == NScheduler::EFetchResult::OK) {
                        if (shared->Instrumentation) {
                            shared->Instrumentation->RetainInput(batch, 0);
                            shared->Instrumentation->Output.Add(batch);
                        }
                        shared->Batches.push_back(batch);
                        continue;
                    }
                    if (fetch == NScheduler::EFetchResult::NO_DATA) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    shared->Status.store(
                        NScheduler::TCteSharedState::EStatus::Ready,
                        std::memory_order_release);
                    return NScheduler::ETaskResult::FINISHED;
                }
            });
        if (materialization->RefCount == 0) {
            throw std::runtime_error(
                "cte materialization: lowered a producer with no consumers");
        }
        auto& completion = AddConn<NScheduler::TBroadcastConnection>(
            1, materialization->RefCount, "cte-completion");
        auto task = std::make_unique<NScheduler::TBlockingTask>(
            code,
            state,
            NScheduler::TInputPort{.Connection = inputConn, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &completion, .Lane = 0});
        auto& node = Graph_.AddOwnedNode(std::move(task));
        const auto execStageId = NewExecStage(
            &consumer, EExecPlanNodeKind::CteProducer);
        MarkNode(node,
            "cte-producer",
            execStageId,
            "cte-producer " + std::to_string(consumer.Def()->Id),
            execStageId);
        for (size_t p = 0; p < lanes; ++p) {
            Graph_.AddEdge(*planOut.Producers[p], node, *inputConn, p, 0);
        }
        return TMaterializedProducer{
            .State = std::move(state),
            .Producer = &node,
            .Completion = &completion,
            .NextConsumerLane = 0,
            .OutputType = std::move(planOut.OutputType),
        };
    }

    template <typename TStageBuilder>
    TLoweredOutput LowerUnary(
        const TOperatorPtr& child,
        std::string stageKind,
        std::string stageGroup,
        TExecStageId execStageId,
        TStageBuilder buildStage,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const size_t lanes = OutputLanes(child);
        auto& childConnRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "unary-input");
        auto childOut = Lower(child, childConnRef);

        TStageDiagnosticsScope diagnosticsScope(Diagnostics_, stageGroup);
        auto stage = buildStage(childOut.OutputType);
        TLoweredOutput result;
        result.OutputType = std::move(stage.OutputType);
        result.Producers.reserve(lanes);
        for (size_t p = 0; p < lanes; ++p) {
            auto task = std::make_unique<NScheduler::TUnaryTask>(
                stage.Code,
                stage.MakeState(p),
                NScheduler::TInputPort{.Connection = &childConnRef, .Lane = p},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = p + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node, stageKind, execStageId, stageKind, execStageId);
            Graph_.AddEdge(*childOut.Producers[p], node, childConnRef, p, p);
            result.Producers.push_back(&node);
        }
        return result;
    }

    template <typename TTailBuilder>
    TLoweredOutput LowerBlocking(
        const TOperatorPtr& child,
        std::string stageKind,
        std::string stageGroup,
        TExecStageId execStageId,
        TTailBuilder buildTail,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const size_t lanes = OutputLanes(child);
        NScheduler::IConnection* inputConn = nullptr;
        if (lanes == 1) {
            inputConn = &AddConn<NScheduler::TOneToOneConnection>(
                1, 1, "blocking-input");
        } else {
            inputConn = &AddConn<NScheduler::TGatherConnection>(
                lanes, 1, "blocking-input-gather");
        }
        auto childOut = Lower(child, *inputConn);

        TStageDiagnosticsScope diagnosticsScope(Diagnostics_, stageGroup);
        TBlockingTail tail = buildTail(childOut.OutputType);
        auto task = std::make_unique<NScheduler::TBlockingTask>(
            std::move(tail.Code),
            tail.MakeState(),
            NScheduler::TInputPort{.Connection = inputConn, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0 + outLaneOffset});
        auto& node = Graph_.AddOwnedNode(std::move(task));
        MarkNode(node, stageKind, execStageId, stageKind + tail.LabelDetail, execStageId);
        for (size_t p = 0; p < lanes; ++p) {
            Graph_.AddEdge(*childOut.Producers[p], node, *inputConn, p, 0);
        }
        return TLoweredOutput{
            .Producers = {&node},
            .OutputType = std::move(tail.OutputType),
        };
    }

    // Builds the aggregate code + partition-local state factory + output type
    // for a given (already-lowered) input type. Kernels are compiled once and
    // shared across all partition states.
    TBlockingTail BuildGroupingSetsAggregateTail(
        const NQumir::NAst::TTypePtr& childType,
        const NQumir::NAst::TStructType& inputType,
        TAggregateOperator& aggregate,
        std::string stage,
        TExecStageId execStageId)
    {
        using namespace NQumir::NAst;
        const size_t numKeys = aggregate.GroupKeys().size();

        // Kernel input = leading i32 __grouping_id__ ++ nullable keys ++ args; the
        // masked batches the driver feeds match this layout column-for-column.
        std::vector<std::pair<std::string, TTypePtr>> extFields;
        extFields.emplace_back("__grouping_id__", std::make_shared<TIntegerType>(TIntegerType::I32));
        for (size_t i = 0; i < inputType.Fields.size(); ++i) {
            auto [name, type] = inputType.Fields[i];
            if (i < numKeys && !IsNullableType(type)) {
                type = std::make_shared<TNullable>(type);
            }
            extFields.emplace_back(name, std::move(type));
        }
        auto extInput = std::make_shared<TStructType>(std::move(extFields));

        std::vector<std::string> extKeys;
        extKeys.reserve(numKeys + 1);
        extKeys.push_back("__grouping_id__");
        for (const auto& key : aggregate.GroupKeys()) {
            extKeys.push_back(key);
        }

        auto spec = NKernel::BuildAggregateKernelSpec(
            *extInput, extKeys, aggregate.Aggs(), ExternalCatalog_.get(),
            aggregate.EmptyInputGroups());
        TKernelCompiler compiler(
            KernelOptions(std::move(stage), execStageId));
        auto kernels = std::make_shared<TAggregateKernels>(compiler.CompileAggregate(spec));
        // The sets share one table and have no group estimate.
        const auto plan = PlanAggregateTable({}, *kernels, Settings_.Aggregate.Table);
        auto sets = aggregate.GroupingSets();

        auto code = std::make_shared<NScheduler::TBlockingCode>(
            [](NScheduler::ITaskState& state, NScheduler::TInputPort& input, TRowSet& output) {
                auto* s = &static_cast<TGroupingSetsBlockingState&>(state);
                if (s->Done) {
                    return NScheduler::ETaskResult::FINISHED;
                }
                TRowSet rowSet{};
                while (true) {
                    auto fetch = input.Fetch(rowSet);
                    if (fetch == NScheduler::EFetchResult::NO_DATA) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    if (fetch == NScheduler::EFetchResult::FINISHED) {
                        s->Done = true;
                        s->Processor.Finish(output);
                        return NScheduler::ETaskResult::OK;
                    }
                    s->Processor.Add(rowSet);
                    Release(&rowSet);
                    rowSet = {};
                }
            });
        return TBlockingTail{
            .Code = std::move(code),
            .MakeState = [kernels, plan, sets, numKeys]() -> std::shared_ptr<NScheduler::ITaskState> {
                return std::make_shared<TGroupingSetsBlockingState>(*kernels, plan, sets, numKeys);
            },
            .OutputType = ComputeAggregateOutputType(
                childType, aggregate.GroupKeys(), aggregate.Aggs(), true,
                !aggregate.EmptyInputGroups().empty()),
            .LabelDetail = AggregateTableLabel(plan),
        };
    }

    TBlockingTail BuildAggregateTail(
        const NQumir::NAst::TTypePtr& childType,
        TAggregateOperator& aggregate,
        std::string stage,
        TExecStageId execStageId,
        const TAggregateTaskEstimate& estimate = {},
        bool hasPrecomputedHash = false)
    {
        auto* inputType =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!inputType) {
            throw std::runtime_error("aggregate input must have TStructType");
        }
        if (!aggregate.GroupingSets().empty()) {
            return BuildGroupingSetsAggregateTail(
                childType, *inputType, aggregate, std::move(stage), execStageId);
        }
        auto spec = NKernel::BuildAggregateKernelSpec(
            *inputType, aggregate.GroupKeys(), aggregate.Aggs(), ExternalCatalog_.get(),
            aggregate.EmptyInputGroups());
        spec.HasPrecomputedHash = hasPrecomputedHash;
        TKernelCompiler compiler(
            KernelOptions(std::move(stage), execStageId));
        auto kernels = std::make_shared<TAggregateKernels>(
            compiler.CompileAggregate(spec));
        const auto plan = PlanAggregateTable(estimate, *kernels, Settings_.Aggregate.Table);
        auto code = std::make_shared<NScheduler::TBlockingCode>(
            [](NScheduler::ITaskState& state, NScheduler::TInputPort& input, TRowSet& output) {
                auto* s = &static_cast<TAggregateBlockingState&>(state);
                if (s->Done) {
                    return NScheduler::ETaskResult::FINISHED;
                }
                TRowSet rowSet{};
                while (true) {
                    auto fetch = input.Fetch(rowSet);
                    if (fetch == NScheduler::EFetchResult::NO_DATA) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    if (fetch == NScheduler::EFetchResult::FINISHED) {
                        s->Done = true;
                        s->Processor.Finish(output);
                        return NScheduler::ETaskResult::OK;
                    }
                    s->Processor.Add(rowSet);
                    Release(&rowSet);
                    rowSet = {};
                }
            });
        return TBlockingTail{
            .Code = std::move(code),
            .MakeState = [kernels, plan]() -> std::shared_ptr<NScheduler::ITaskState> {
                return std::make_shared<TAggregateBlockingState>(*kernels, plan);
            },
            .OutputType = spec.OutputSchema,
            .LabelDetail = AggregateTableLabel(plan),
        };
    }

    // Combine phase for a parallelized grouping-sets aggregate: a plain grouped
    // aggregate over the partials, keyed by the synthetic `__grouping_id__`
    // (leading key that keeps masked-NULL sets distinct) plus the original keys.
    std::shared_ptr<TAggregateOperator> BuildGroupingSetsCombineAggregate(
        TAggregateOperator& aggregate)
    {
        std::vector<std::string> keys;
        keys.reserve(aggregate.GroupKeys().size() + 1);
        keys.push_back("__grouping_id__");
        for (const auto& key : aggregate.GroupKeys()) {
            keys.push_back(key);
        }
        return BuildAggregateCombineOperator(aggregate, std::move(keys));
    }

    // child[N] -> partial-aggregate[N] -> gather(N->1) -> final-combine[1].
    TLoweredOutput LowerAggregateCascade(
        TAggregateOperator& aggregate,
        size_t lanes,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        auto& childRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "aggregate-cascade-input");
        auto childOut = Lower(aggregate.Input(), childRef);

        const auto partialStageId = NewAggregateStage(
            &aggregate, EAggregatePhase::Partial);
        const auto partialGroup = StageLabel(
            "aggregate-partial", partialStageId);
        TBlockingTail partialTail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, partialGroup);
                return BuildAggregateTail(
                    childOut.OutputType, aggregate, partialGroup, partialStageId);
            }();
        auto partialType = partialTail.OutputType;

        auto& gatherRef = AddConn<NScheduler::TGatherConnection>(
            lanes, 1, "aggregate-cascade-gather");

        std::vector<NScheduler::TTaskNode*> partialNodes;
        partialNodes.reserve(lanes);
        for (size_t m = 0; m < lanes; ++m) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                partialTail.Code,
                partialTail.MakeState(),
                NScheduler::TInputPort{.Connection = &childRef, .Lane = m},
                NScheduler::TOutputPort{.Connection = &gatherRef, .Lane = m});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "aggregate",
                partialStageId,
                "aggregate partial" + partialTail.LabelDetail,
                partialStageId);
            Graph_.AddEdge(*childOut.Producers[m], node, childRef, m, m);
            partialNodes.push_back(&node);
        }

        auto combineAgg = BuildAggregateCombineOperator(
            aggregate, aggregate.GroupKeys());
        const auto combineStageId = NewAggregateStage(
            combineAgg.get(), EAggregatePhase::Final, combineAgg);
        const auto combineGroup = StageLabel(
            "aggregate-combine", combineStageId);
        TBlockingTail combineTail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, combineGroup);
                return BuildAggregateTail(
                    partialType, *combineAgg, combineGroup, combineStageId);
            }();
        auto finalTask = std::make_unique<NScheduler::TBlockingTask>(
            std::move(combineTail.Code),
            combineTail.MakeState(),
            NScheduler::TInputPort{.Connection = &gatherRef, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0 + outLaneOffset});
        auto& finalNode = Graph_.AddOwnedNode(std::move(finalTask));
        MarkNode(finalNode,
            "aggregate",
            combineStageId,
            "aggregate combine" + combineTail.LabelDetail,
            combineStageId);
        for (size_t m = 0; m < lanes; ++m) {
            Graph_.AddEdge(*partialNodes[m], finalNode, gatherRef, m, 0);
        }

        return TLoweredOutput{
            .Producers = {&finalNode},
            .OutputType = std::move(combineTail.OutputType),
        };
    }

    // child[N] -> partial grouping-sets aggregate[N] -> hash-shuffle by
    // (__grouping_id__ + keys) into `parts` -> grouped combine[parts]. Raw input
    // cannot be shuffled (coarse rollup sets group by a key prefix, scattering a
    // group across partitions); the partial materializes the masked keys and set
    // id first, so the (already small) partials shuffle into disjoint groups.
    TLoweredOutput LowerGroupingSetsCascade(
        TAggregateOperator& aggregate,
        size_t childLanes,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        // Must match OutputLanes: with no group keys (only the empty set) there is
        // nothing to shuffle by, so the partials gather (N->1) into one global
        // group and the aggregate stays single-lane.
        const size_t parts =
            aggregate.GroupKeys().empty() ? 1 : JoinPartitions(childLanes);

        auto& childRef = AddConn<NScheduler::TOneToOneConnection>(
            childLanes, childLanes, "grouping-sets-partial-input");
        auto childOut = Lower(aggregate.Input(), childRef);

        const auto partialStageId = NewAggregateStage(
            &aggregate, EAggregatePhase::Partial);
        const auto partialGroup = StageLabel(
            "aggregate-partial", partialStageId);
        TBlockingTail partialTail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, partialGroup);
                return BuildAggregateTail(
                    childOut.OutputType, aggregate, partialGroup, partialStageId);
            }();
        auto partialType = partialTail.OutputType;
        auto* partialStruct =
            static_cast<NQumir::NAst::TStructType*>(partialType.get());
        if (!partialStruct) {
            throw std::runtime_error("grouping-sets partial output must have TStructType");
        }

        auto& partialRef = AddConn<NScheduler::TOneToOneConnection>(
            childLanes, childLanes, "grouping-sets-partial");
        std::vector<NScheduler::TTaskNode*> partialNodes;
        partialNodes.reserve(childLanes);
        for (size_t i = 0; i < childLanes; ++i) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                partialTail.Code,
                partialTail.MakeState(),
                NScheduler::TInputPort{.Connection = &childRef, .Lane = i},
                NScheduler::TOutputPort{.Connection = &partialRef, .Lane = i});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(
                node, "aggregate", partialStageId,
                "aggregate partial" + partialTail.LabelDetail, partialStageId);
            Graph_.AddEdge(*childOut.Producers[i], node, childRef, i, i);
            partialNodes.push_back(&node);
        }

        auto combineAgg = BuildGroupingSetsCombineAggregate(aggregate);
        TKernelCompiler hashCompiler(KernelOptions(partialGroup, partialStageId));
        auto groupHash = hashCompiler.CompileAggregateHash(
            *partialStruct, combineAgg->GroupKeys());
        auto hashCode = MakeHashShuffleCode(std::move(groupHash), partialType);
        TLoweredOutput partialOut{
            .Producers = partialNodes,
            .OutputType = partialType,
        };
        auto exchange = BuildRepartitionExchange(
            partialOut,
            partialRef,
            childLanes,
            parts,
            std::move(hashCode),
            "grouping-sets-repartition",
            {.Keys = combineAgg->GroupKeys()});

        const auto combineStageId = NewAggregateStage(
            combineAgg.get(), EAggregatePhase::Final, combineAgg);
        const auto combineGroup = StageLabel(
            "aggregate-combine", combineStageId);
        TBlockingTail combineTail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, combineGroup);
                return BuildAggregateTail(
                    partialType, *combineAgg, combineGroup, combineStageId,
                    /*estimate=*/{}, /*hasPrecomputedHash=*/true);
            }();

        TLoweredOutput result;
        result.OutputType = combineTail.OutputType;
        result.Producers.reserve(parts);
        for (size_t m = 0; m < parts; ++m) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                combineTail.Code,
                combineTail.MakeState(),
                NScheduler::TInputPort{
                    .Connection = exchange.Connection, .Lane = m},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = m + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(
                node, "aggregate", combineStageId,
                "aggregate combine" + combineTail.LabelDetail, combineStageId);
            for (size_t s = 0; s < childLanes; ++s) {
                Graph_.AddEdge(
                    *exchange.Nodes[s], node, *exchange.Connection, s, m);
            }
            result.Producers.push_back(&node);
        }
        return result;
    }

    // child[N] -> local partial aggregate[N] -> hash repartition exchange
    // (N->parts) -> final combine aggregate[parts]. The two aggregate phases
    // have independent executable-stage identities; the exchange connection
    // carries its partitioning contract so a distributed scheduler can replace
    // only the transport between them.
    TLoweredOutput LowerGroupedAggregateCascade(
        TAggregateOperator& aggregate,
        size_t childLanes,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const size_t parts = AggregatePartitions(aggregate, childLanes);
        auto& childRef = AddConn<NScheduler::TOneToOneConnection>(
            childLanes, childLanes, "aggregate-partial-input");
        auto childOut = Lower(aggregate.Input(), childRef);

        const auto partialStageId = NewAggregateStage(
            &aggregate, EAggregatePhase::Partial);
        const auto partialGroup = StageLabel(
            "aggregate-partial", partialStageId);
        TBlockingTail partialTail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, partialGroup);
                return BuildAggregateTail(
                    childOut.OutputType,
                    aggregate,
                    partialGroup,
                    partialStageId,
                    EstimateAggregateTask(aggregate, childLanes));
            }();
        auto partialType = partialTail.OutputType;
        auto* partialStruct = static_cast<NQumir::NAst::TStructType*>(
            partialType.get());
        if (!partialStruct) {
            throw std::runtime_error(
                "partial aggregate output must have TStructType");
        }

        auto& partialRef = AddConn<NScheduler::TOneToOneConnection>(
            childLanes, childLanes, "aggregate-partial-output");
        TLoweredOutput partialOut;
        partialOut.OutputType = partialType;
        partialOut.Producers.reserve(childLanes);
        for (size_t lane = 0; lane < childLanes; ++lane) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                partialTail.Code,
                partialTail.MakeState(),
                NScheduler::TInputPort{
                    .Connection = &childRef, .Lane = lane},
                NScheduler::TOutputPort{
                    .Connection = &partialRef, .Lane = lane});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(
                node,
                "aggregate",
                partialStageId,
                "aggregate partial" + partialTail.LabelDetail,
                partialStageId);
            Graph_.AddEdge(
                *childOut.Producers[lane], node, childRef, lane, lane);
            partialOut.Producers.push_back(&node);
        }

        auto combineAgg = BuildAggregateCombineOperator(
            aggregate, aggregate.GroupKeys());
        const auto finalStageId = NewAggregateStage(
            combineAgg.get(), EAggregatePhase::Final, combineAgg);
        const auto finalGroup = StageLabel(
            "aggregate-final", finalStageId);

        TKernelCompiler hashCompiler(
            KernelOptions(partialGroup, partialStageId));
        auto groupHash = hashCompiler.CompileAggregateHash(
            *partialStruct, combineAgg->GroupKeys());
        auto exchange = BuildRepartitionExchange(
            partialOut,
            partialRef,
            childLanes,
            parts,
            MakeHashShuffleCode(std::move(groupHash), partialType),
            "aggregate-repartition",
            {.Keys = combineAgg->GroupKeys()});

        TBlockingTail finalTail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, finalGroup);
                return BuildAggregateTail(
                    partialType,
                    *combineAgg,
                    finalGroup,
                    finalStageId,
                    EstimateAggregateTask(aggregate, parts),
                    /*hasPrecomputedHash=*/true);
            }();

        TLoweredOutput result;
        result.OutputType = finalTail.OutputType;
        result.Producers.reserve(parts);
        for (size_t partition = 0; partition < parts; ++partition) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                finalTail.Code,
                finalTail.MakeState(),
                NScheduler::TInputPort{
                    .Connection = exchange.Connection, .Lane = partition},
                NScheduler::TOutputPort{
                    .Connection = &outConn,
                    .Lane = partition + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(
                node,
                "aggregate",
                finalStageId,
                "aggregate final" + finalTail.LabelDetail,
                finalStageId);
            for (size_t lane = 0; lane < childLanes; ++lane) {
                Graph_.AddEdge(
                    *exchange.Nodes[lane],
                    node,
                    *exchange.Connection,
                    lane,
                    partition);
            }
            result.Producers.push_back(&node);
        }
        return result;
    }

    TLoweredOutput LowerAggregate(
        TAggregateOperator& aggregate,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const size_t childLanes = OutputLanes(aggregate.Input());
        // Non-parallel input: single aggregate, nothing to parallelize.
        if (childLanes <= 1) {
            auto execStageId = NewExecStage(
                &aggregate, EExecPlanNodeKind::Aggregate);
            auto group = StageLabel("aggregate", execStageId);
            return LowerBlocking(
                aggregate.Input(),
                "aggregate",
                group,
                execStageId,
                [&, group](const NQumir::NAst::TTypePtr& childType) {
                    return BuildAggregateTail(
                        childType, aggregate, group, execStageId,
                        EstimateAggregateTask(aggregate, 1));
                },
                outConn,
                outLaneOffset);
        }

        // Grouping sets over a parallel input: partial -> shuffle -> combine
        // cascade (the partial materializes the masked keys so they can shuffle).
        if (!aggregate.GroupingSets().empty()) {
            return LowerGroupingSetsCascade(aggregate, childLanes, outConn, outLaneOffset);
        }

        // Ungrouped or global (`__group__`) aggregate over a parallel input.
        // Opt-in cascade (partial -> gather -> combine) parallelizes the
        // aggregate; otherwise gather every lane into a single aggregate, which
        // is faster for cheap aggregates (the common case).
        if (aggregate.GroupKeys().empty() || aggregate.IsGlobal()) {
            if (Settings_.Aggregate.CascadeGlobal) {
                return LowerAggregateCascade(aggregate, childLanes, outConn, outLaneOffset);
            }
            auto execStageId = NewExecStage(
                &aggregate, EExecPlanNodeKind::Aggregate);
            auto group = StageLabel("aggregate", execStageId);
            return LowerBlocking(
                aggregate.Input(),
                "aggregate",
                group,
                execStageId,
                [&, group](const NQumir::NAst::TTypePtr& childType) {
                    return BuildAggregateTail(
                        childType, aggregate, group, execStageId);
                },
                outConn,
                outLaneOffset);
        }

        if (NScheduler::ShouldUsePartialAggregate(aggregate, Settings_)) {
            return LowerGroupedAggregateCascade(
                aggregate, childLanes, outConn, outLaneOffset);
        }

        // Grouped aggregate: hash-shuffle the input by group key into `parts`
        // partition-local aggregates. Matching keys land in one partition, so
        // each computes complete groups; the parent gathers the partitions.
        const size_t parts = AggregatePartitions(aggregate, childLanes);
        auto& childConnRef = AddConn<NScheduler::TOneToOneConnection>(
            childLanes, childLanes, "aggregate-shuffle-input");
        auto childOut = Lower(aggregate.Input(), childConnRef);

        auto childType = childOut.OutputType;
        auto* childStruct =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!childStruct) {
            throw std::runtime_error("aggregate input must have TStructType");
        }
        const auto execStageId = NewExecStage(
            &aggregate, EExecPlanNodeKind::Aggregate);
        const auto aggregateGroup = StageLabel("aggregate", execStageId);
        TKernelCompiler hashCompiler(KernelOptions(aggregateGroup, execStageId));
        auto groupHash = hashCompiler.CompileAggregateHash(
            *childStruct, aggregate.GroupKeys());
        TBlockingTail tail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, aggregateGroup);
                return BuildAggregateTail(
                    childType, aggregate, aggregateGroup, execStageId,
                    EstimateAggregateTask(aggregate, parts),
                    /*hasPrecomputedHash=*/true);
            }();

        auto hashCode = MakeHashShuffleCode(std::move(groupHash), childType);
        auto exchange = BuildRepartitionExchange(
            childOut,
            childConnRef,
            childLanes,
            parts,
            std::move(hashCode),
            "aggregate-repartition",
            {.Keys = aggregate.GroupKeys()});

        TLoweredOutput result;
        result.OutputType = tail.OutputType;
        result.Producers.reserve(parts);
        for (size_t m = 0; m < parts; ++m) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                tail.Code,
                tail.MakeState(),
                NScheduler::TInputPort{
                    .Connection = exchange.Connection, .Lane = m},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = m + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "aggregate",
                execStageId,
                "aggregate" + tail.LabelDetail,
                execStageId);
            for (size_t s = 0; s < childLanes; ++s) {
                Graph_.AddEdge(
                    *exchange.Nodes[s], node, *exchange.Connection, s, m);
            }
            result.Producers.push_back(&node);
        }
        return result;
    }

    TLoweredOutput LowerLimit(
        TLimitOperator& limit,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const auto execStageId = NewExecStage(
            &limit, EExecPlanNodeKind::Limit);
        return LowerBlocking(
            limit.Input(),
            "limit",
            StageLabel("limit", execStageId),
            execStageId,
            [&](const NQumir::NAst::TTypePtr& childType) {
                const int64_t limitValue = limit.Limit();
                const int64_t offsetValue = limit.Offset();
                return TBlockingTail{
                    .Code = MakeLimitBlockingCode(),
                    .MakeState = [limitValue, offsetValue]()
                        -> std::shared_ptr<NScheduler::ITaskState>
                    {
                        return std::make_shared<TLimitBlockingState>(
                            limitValue, offsetValue);
                    },
                    .OutputType = childType,
                };
            },
            outConn,
            outLaneOffset);
    }

    TLoweredOutput LowerLateMaterialize(
        TLateMaterializeOperator& late,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const auto execStageId = NewExecStage(
            &late, EExecPlanNodeKind::LateMaterialize);
        auto columns = late.Columns();
        auto source = ResolveLateMaterializationSource(late);
        if (!source.Source.EmitsRowId()) {
            throw std::runtime_error(
                "late materialize source has not been bound to emit locators");
        }
        auto* lookup = &source.Lookup;
        const auto locator = late.LocatorColumn();
        const auto outputType = late.OutputColumns();
        const size_t childLanes = OutputLanes(late.Input());
        NScheduler::IConnection* childConnection = nullptr;
        if (childLanes == 1) {
            childConnection = &AddConn<NScheduler::TOneToOneConnection>(
                1, 1, "late-lookup-input");
        } else {
            childConnection = &AddConn<NScheduler::TGatherConnection>(
                childLanes, 1, "late-lookup-input-gather");
        }
        auto childOut = Lower(late.Input(), *childConnection);

        auto* input = static_cast<NQumir::NAst::TStructType*>(
            childOut.OutputType.get());
        if (!input) {
            throw std::runtime_error(
                "late materialize input must have TStructType");
        }
        int32_t locatorColumn = -1;
        for (size_t i = 0; i < input->Fields.size(); ++i) {
            if (input->Fields[i].first == locator) {
                locatorColumn = static_cast<int32_t>(i);
                break;
            }
        }
        if (locatorColumn < 0) {
            throw std::runtime_error(
                "late materialize input is missing locator column '" +
                locator + "'");
        }

        std::vector<std::string> physicalColumns;
        physicalColumns.reserve(columns.size());
        for (const auto& column : columns) {
            physicalColumns.push_back(column.PhysicalName);
        }
        if (physicalColumns.empty()) {
            throw std::runtime_error(
                "late materialize requires at least one output column");
        }
        // Column decode costs can differ substantially. Four runnable ranges
        // per worker let work stealing smooth that skew while keeping all I/O
        // in the scheduler pool. With one worker oversubscription cannot help.
        constexpr size_t lookupTasksPerWorker = 4;
        const size_t workers = std::max<size_t>(
            Settings_.Scheduler.WorkerCount, 1);
        size_t maxThreadedLookupTasks = 1;
        if (workers > 1) {
            maxThreadedLookupTasks = workers >
                    std::numeric_limits<size_t>::max() / lookupTasksPerWorker
                ? std::numeric_limits<size_t>::max()
                : workers * lookupTasksPerWorker;
        }
        const size_t maxLookupTasks =
            Settings_.Scheduler.Mode ==
                    NScheduler::EExecutionMode::ThreadedScheduler
                ? maxThreadedLookupTasks
                : 1;
        const size_t lookupTaskCount = std::min(
            physicalColumns.size(), maxLookupTasks);

        NScheduler::IConnection* requests = childConnection;
        NScheduler::TTaskNode* broadcastNode = nullptr;
        if (lookupTaskCount > 1) {
            auto& broadcast = AddConn<NScheduler::TBroadcastConnection>(
                1, lookupTaskCount, "late-lookup-requests");
            // TBroadcastConnection requires one producer. This bridge turns
            // the already gathered locator stream into that producer.
            auto forwardCode = std::make_shared<NScheduler::TUnaryCode>(
                [](void*, TRowSet&) {});
            auto forward = std::make_unique<NScheduler::TUnaryTask>(
                forwardCode,
                std::shared_ptr<void>{},
                NScheduler::TInputPort{
                    .Connection = childConnection, .Lane = 0},
                NScheduler::TOutputPort{
                    .Connection = &broadcast, .Lane = 0});
            auto& forwardNode = Graph_.AddOwnedNode(std::move(forward));
            MarkNode(
                forwardNode,
                "late-lookup-broadcast",
                NewTaskGroup(),
                "late-lookup-broadcast");
            for (size_t lane = 0; lane < childLanes; ++lane) {
                Graph_.AddEdge(
                    *childOut.Producers[lane],
                    forwardNode,
                    *childConnection,
                    lane,
                    0);
            }
            requests = &broadcast;
            broadcastNode = &forwardNode;
        }

        auto& results = AddConn<NScheduler::TOneToOneConnection>(
            lookupTaskCount, lookupTaskCount, "late-lookup-results");
        auto lookupCode = MakeDrainBlockingCode<TLateMaterializeBlockingState>();
        std::vector<NScheduler::TTaskNode*> lookupNodes;
        lookupNodes.reserve(lookupTaskCount);
        for (size_t taskIndex = 0; taskIndex < lookupTaskCount; ++taskIndex) {
            // The lookup API exposes no per-column byte estimate, so divide
            // adjacent output columns by count. Oversubscription above limits
            // the impact of uneven ranges without coupling lowering to Parquet.
            const size_t begin =
                physicalColumns.size() * taskIndex / lookupTaskCount;
            const size_t end =
                physicalColumns.size() * (taskIndex + 1) / lookupTaskCount;
            const std::span<const std::string> taskColumns(
                physicalColumns.data() + begin, end - begin);
            auto reader = lookup->CompileReader(taskColumns);
            if (!reader) {
                throw std::runtime_error(
                    "late materialize source failed to compile row reader");
            }
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                lookupCode,
                std::make_shared<TLateMaterializeBlockingState>(
                    std::move(reader), locatorColumn),
                NScheduler::TInputPort{
                    .Connection = requests, .Lane = taskIndex},
                NScheduler::TOutputPort{
                    .Connection = &results, .Lane = taskIndex});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(
                node,
                "late-lookup",
                execStageId,
                "late-lookup",
                execStageId);
            if (broadcastNode) {
                Graph_.AddEdge(
                    *broadcastNode, node, *requests, 0, taskIndex);
            } else {
                for (size_t lane = 0; lane < childLanes; ++lane) {
                    Graph_.AddEdge(
                        *childOut.Producers[lane],
                        node,
                        *childConnection,
                        lane,
                        0);
                }
            }
            lookupNodes.push_back(&node);
        }

        auto mergeCode = std::make_shared<NScheduler::TMergeCode>(
            [](NScheduler::ITaskState& state,
               std::vector<NScheduler::TInputPort>& inputs,
               TRowSet& output)
            {
                auto* merge = &static_cast<TLateMaterializeMergeState&>(state);
                if (merge->Emitted) {
                    return NScheduler::ETaskResult::FINISHED;
                }

                for (size_t i = 0; i < inputs.size(); ++i) {
                    if (merge->Received[i] || merge->Finished[i]) {
                        continue;
                    }
                    TRowSet partition{};
                    switch (inputs[i].Fetch(partition)) {
                        case NScheduler::EFetchResult::OK:
                            merge->Partitions[i] = partition;
                            merge->Received[i] = true;
                            break;
                        case NScheduler::EFetchResult::NO_DATA:
                            break;
                        case NScheduler::EFetchResult::FINISHED:
                            merge->Finished[i] = true;
                            break;
                    }
                }

                const bool allReceived = std::ranges::all_of(
                    merge->Received, [](bool value) { return value; });
                if (allReceived) {
                    MergeLateMaterializedColumns(merge->Partitions, output);
                    std::ranges::fill(merge->Received, false);
                    merge->Emitted = true;
                    return NScheduler::ETaskResult::OK;
                }

                const bool allFinished = std::ranges::all_of(
                    merge->Finished, [](bool value) { return value; });
                const bool anyReceived = std::ranges::any_of(
                    merge->Received, [](bool value) { return value; });
                const bool anyFinished = std::ranges::any_of(
                    merge->Finished, [](bool value) { return value; });
                if (allFinished && !anyReceived) {
                    return NScheduler::ETaskResult::FINISHED;
                }
                if (anyReceived && anyFinished) {
                    throw std::runtime_error(
                        "late materialize: lookup partitions returned "
                        "different result counts");
                }
                return NScheduler::ETaskResult::NEED_DATA;
            });
        std::vector<NScheduler::TInputPort> mergeInputs;
        mergeInputs.reserve(lookupTaskCount);
        for (size_t taskIndex = 0; taskIndex < lookupTaskCount; ++taskIndex) {
            mergeInputs.push_back(NScheduler::TInputPort{
                .Connection = &results, .Lane = taskIndex});
        }
        auto mergeTask = std::make_unique<NScheduler::TMergeTask>(
            mergeCode,
            std::make_shared<TLateMaterializeMergeState>(lookupTaskCount),
            std::move(mergeInputs),
            NScheduler::TOutputPort{
                .Connection = &outConn, .Lane = outLaneOffset});
        auto& mergeNode = Graph_.AddOwnedNode(std::move(mergeTask));
        MarkNode(
            mergeNode,
            "late-materialize",
            execStageId,
            "late-materialize",
            execStageId);
        for (size_t taskIndex = 0; taskIndex < lookupTaskCount; ++taskIndex) {
            Graph_.AddEdge(
                *lookupNodes[taskIndex],
                mergeNode,
                results,
                taskIndex,
                taskIndex);
        }

        return TLoweredOutput{
            .Producers = {&mergeNode},
            .OutputType = outputType,
        };
    }

    TLoweredOutput LowerSortLike(
        const TOperatorPtr& input,
        const std::vector<TSortKey>& sortKeys,
        std::string_view kernelName,
        std::optional<int64_t> topLimit,
        const void* sortOp,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const auto execStageId = NewExecStage(
            static_cast<const IOperator*>(sortOp),
            kernelName == "top-sort"
                ? EExecPlanNodeKind::TopSort
                : EExecPlanNodeKind::Sort);
        auto group = StageLabel(kernelName, execStageId);
        return LowerBlocking(
            input,
            std::string(kernelName),
            group,
            execStageId,
            [&, group](const NQumir::NAst::TTypePtr& childType) {
                auto* inputType =
                    static_cast<NQumir::NAst::TStructType*>(childType.get());
                if (!inputType) {
                    throw std::runtime_error(
                        "sort input must have TStructType");
                }
                auto runtime = BuildSortRuntimeProcess(
                    *inputType, sortKeys, kernelName,
                    KernelOptions(group, execStageId));
                return MakeSortTail(
                    childType, sortKeys,
                    std::move(runtime.KeyColumns),
                    std::move(runtime.RadixKernel), topLimit);
            },
            outConn,
            outLaneOffset);
    }

    struct TSortMergeResult {
        NScheduler::TTaskNode* Merge = nullptr;
        NQumir::NAst::TTypePtr OutputType;
    };

    // Builds `child[N] -> local-(top-)sort[N] -> merge`, writing the merged
    // stream to mergeOut. When topLimit is set each local task is a top-sort
    // keeping its local top-K; otherwise a full local sort. Assumes N > 1.
    TSortMergeResult BuildSortMerge(
        const TOperatorPtr& input,
        const std::vector<TSortKey>& sortKeys,
        std::string_view kernelName,
        std::optional<int64_t> topLimit,
        const void* sortOp,
        NScheduler::IConnection& mergeOut,
        size_t mergeOutLaneOffset)
    {
        const size_t lanes = OutputLanes(input);
        const auto execStageId = NewExecStage(
            static_cast<const IOperator*>(sortOp),
            kernelName == "top-sort"
                ? EExecPlanNodeKind::TopSort
                : EExecPlanNodeKind::Sort);
        auto& childConnRef = AddConn<NScheduler::TOneToOneConnection>(
            lanes, lanes, "sort-merge-input");
        auto childOut = Lower(input, childConnRef);

        auto childType = childOut.OutputType;
        auto* inputType =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!inputType) {
            throw std::runtime_error("sort input must have TStructType");
        }
        auto localSortGroup = StageLabel(kernelName, execStageId);
        TSortRuntimeProcess runtime =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, localSortGroup);
                return BuildSortRuntimeProcess(
                    *inputType, sortKeys, kernelName,
                    KernelOptions(localSortGroup, execStageId));
            }();
        auto keys = sortKeys;
        auto keyColumns = runtime.KeyColumns;

        // Per-lane local (top-)sort producing sorted runs for the merge.
        auto localTail = MakeSortTail(
            childType, keys, keyColumns,
            std::move(runtime.RadixKernel), topLimit);

        // One local (top-)sort per input lane, each writing a sorted run.
        std::vector<NScheduler::IConnection*> runConns;
        std::vector<NScheduler::TInputPort> mergeInputs;
        std::vector<NScheduler::TTaskNode*> localSorts;
        runConns.reserve(lanes);
        mergeInputs.reserve(lanes);
        localSorts.reserve(lanes);
        for (size_t i = 0; i < lanes; ++i) {
            auto& runRef = AddConn<NScheduler::TOneToOneConnection>(
                1, 1, "sort-run-" + std::to_string(i));
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                localTail.Code,
                localTail.MakeState(),
                NScheduler::TInputPort{.Connection = &childConnRef, .Lane = i},
                NScheduler::TOutputPort{.Connection = &runRef, .Lane = 0});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                std::string(kernelName),
                execStageId,
                std::string(kernelName),
                execStageId);
            Graph_.AddEdge(*childOut.Producers[i], node, childConnRef, i, i);
            runConns.push_back(&runRef);
            mergeInputs.push_back(
                NScheduler::TInputPort{.Connection = &runRef, .Lane = 0});
            localSorts.push_back(&node);
        }

        auto mergeCode = std::make_shared<NScheduler::TMergeCode>(
            [](NScheduler::ITaskState& state,
               std::vector<NScheduler::TInputPort>& inputs,
               TRowSet& output)
            {
                auto* s = &static_cast<TMergeState&>(state);
                if (!s->InputsDone) {
                    bool allDone = true;
                    for (size_t i = 0; i < inputs.size(); ++i) {
                        if (s->RunFinished[i]) {
                            continue;
                        }
                        for (;;) {
                            TRowSet batch{};
                            auto fetch = inputs[i].Fetch(batch);
                            if (fetch == NScheduler::EFetchResult::NO_DATA) {
                                allDone = false;
                                break;
                            }
                            if (fetch == NScheduler::EFetchResult::FINISHED) {
                                s->RunFinished[i] = true;
                                break;
                            }
                            s->Processor.Add(batch, i);
                        }
                    }
                    if (!allDone) {
                        return NScheduler::ETaskResult::NEED_DATA;
                    }
                    s->Processor.Finish();
                    s->InputsDone = true;
                }
                if (s->Processor.Next(output)) {
                    return NScheduler::ETaskResult::OK;
                }
                return NScheduler::ETaskResult::FINISHED;
            });

        auto merge = std::make_unique<NScheduler::TMergeTask>(
            mergeCode,
            std::make_shared<TMergeState>(childType, keys, keyColumns, lanes),
            std::move(mergeInputs),
            NScheduler::TOutputPort{.Connection = &mergeOut, .Lane = 0 + mergeOutLaneOffset});
        auto& mergeNode = Graph_.AddOwnedNode(std::move(merge));
        MarkNode(mergeNode,
            "merge",
            NewTaskGroup(),
            "merge");
        for (size_t i = 0; i < lanes; ++i) {
            Graph_.AddEdge(*localSorts[i], mergeNode, *runConns[i], 0, 0);
        }

        return TSortMergeResult{.Merge = &mergeNode, .OutputType = childType};
    }

    // Partitioned sort: sort each input lane locally, then k-way merge the
    // sorted runs (`sort -> merge`). Falls back to a single gathered sort when
    // there is only one input lane.
    TLoweredOutput LowerSort(
        TSortOperator& sort,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        if (OutputLanes(sort.Input()) <= 1) {
            return LowerSortLike(
                sort.Input(), sort.Keys(), "sort", std::nullopt, &sort, outConn, outLaneOffset);
        }
        auto merged = BuildSortMerge(
            sort.Input(), sort.Keys(), "sort", std::nullopt, &sort, outConn, outLaneOffset);
        return TLoweredOutput{
            .Producers = {merged.Merge},
            .OutputType = std::move(merged.OutputType),
        };
    }

    // Partitioned top-sort: local top-K per lane, k-way merge the sorted runs,
    // then a final limit (`top-sort -> merge -> limit`). Falls back to a single
    // gathered top-sort when there is only one input lane.
    TLoweredOutput LowerTopSort(
        TTopSortOperator& sort,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        if (OutputLanes(sort.Input()) <= 1) {
            return LowerSortLike(
                sort.Input(), sort.Keys(), "top-sort", sort.Limit(), &sort, outConn, outLaneOffset);
        }
        auto& mergeConnRef = AddConn<NScheduler::TOneToOneConnection>(
            1, 1, "top-sort-merge-output");
        auto merged = BuildSortMerge(
            sort.Input(), sort.Keys(), "top-sort", sort.Limit(), &sort, mergeConnRef, 0);

        auto limitCode = MakeLimitBlockingCode();
        const int64_t limit = sort.Limit();
        auto limitTask = std::make_unique<NScheduler::TBlockingTask>(
            limitCode,
            std::make_shared<TLimitBlockingState>(limit, 0),
            NScheduler::TInputPort{.Connection = &mergeConnRef, .Lane = 0},
            NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0 + outLaneOffset});
        auto& limitNode = Graph_.AddOwnedNode(std::move(limitTask));
        MarkNode(limitNode,
            "limit",
            NewTaskGroup(),
            "limit");
        Graph_.AddEdge(*merged.Merge, limitNode, mergeConnRef, 0, 0);

        return TLoweredOutput{
            .Producers = {&limitNode},
            .OutputType = std::move(merged.OutputType),
        };
    }

    // Keyless (nested-loop) join with a scalar (broadcast) right side:
    //   vector[N] x Broadcast(scalar) -> local cross[N] -> filter(residual) ->
    //   parent gather. The scalar side is lowered, gathered to one lane, and
    //   forwarded into a broadcast connection replicated to every vector lane.
    //   RIGHT / FULL gather the vector side to one lane (KeylessJoinLanes).
    //   Only an inner join filters the product by its residual; the other
    //   types evaluate the residual in the kernel, where it decides matches.
    TLoweredOutput LowerCrossJoin(
        TJoinOperator& join,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const auto crossStageId = NewExecStage(
            &join, EExecPlanNodeKind::CrossJoin);
        const size_t leftLanes = OutputLanes(join.Left());
        const size_t lanes = KeylessJoinLanes(join);
        const bool gatherVector = lanes != leftLanes;

        // Vector (streamed, partitioned) side.
        NScheduler::IConnection* vectorRef = nullptr;
        if (gatherVector) {
            vectorRef = &AddConn<NScheduler::TGatherConnection>(
                leftLanes, lanes, "cross-vector-gather");
        } else {
            vectorRef = &AddConn<NScheduler::TOneToOneConnection>(
                lanes, lanes, "cross-vector-input");
        }
        auto vectorOut = Lower(join.Left(), *vectorRef);

        const size_t scalarLanes = OutputLanes(join.Right());
        TLoweredOutput scalarOut;
        NScheduler::IConnection* scalarInput = nullptr;
        NScheduler::TTaskNode* scalarBroadcastProducer = nullptr;
        const bool directScalar = lanes == 1 && scalarLanes == 1;
        if (directScalar) {
            scalarInput = &AddConn<NScheduler::TOneToOneConnection>(
                1, 1, "cross-right-input");
            scalarOut = Lower(join.Right(), *scalarInput);
        } else {
            // Scalar (buffered, broadcast) side: lower it, gather to one lane,
            // then forward into a broadcast connection replicated to every
            // vector lane.
            auto& scalarGatherRef = AddConn<NScheduler::TGatherConnection>(
                scalarLanes, 1, "cross-scalar-gather");
            scalarOut = Lower(join.Right(), scalarGatherRef);

            auto& broadcastRef = AddConn<NScheduler::TBroadcastConnection>(
                1, lanes, "cross-scalar-broadcast");

            auto fwdCode = std::make_shared<NScheduler::TUnaryCode>(
                [](void*, TRowSet&) {});
            auto fwd = std::make_unique<NScheduler::TUnaryTask>(
                fwdCode,
                std::make_shared<int>(0),
                NScheduler::TInputPort{.Connection = &scalarGatherRef, .Lane = 0},
                NScheduler::TOutputPort{.Connection = &broadcastRef, .Lane = 0});
            auto& fwdNode = Graph_.AddOwnedNode(std::move(fwd));
            MarkNode(fwdNode,
                "broadcast",
                NewTaskGroup(),
                "broadcast");
            for (size_t m = 0; m < scalarLanes; ++m) {
                Graph_.AddEdge(
                    *scalarOut.Producers[m], fwdNode, scalarGatherRef, m, 0);
            }
            scalarInput = &broadcastRef;
            scalarBroadcastProducer = &fwdNode;
        }

        auto leftType = vectorOut.OutputType;
        auto rightType = scalarOut.OutputType;
        auto crossTypeExp =
            ComputeJoinOutputType(leftType, rightType, join.JoinType());
        if (!crossTypeExp) {
            throw std::runtime_error(
                "cross join: " + crossTypeExp.error().ToString());
        }
        auto crossType = *crossTypeExp;

        auto* leftStruct = static_cast<NQumir::NAst::TStructType*>(leftType.get());
        auto* rightStruct = static_cast<NQumir::NAst::TStructType*>(rightType.get());
        if (!leftStruct || !rightStruct) {
            throw std::runtime_error("scheduler cross join inputs must have TStructType");
        }
        const bool filtersProduct =
            join.JoinType() == EJoinType::Inner && join.Filter() != nullptr;
        auto kernelSpec = NKernel::BuildCrossJoinKernelSpec(
            *leftStruct, *rightStruct, join.JoinType(),
            join.JoinType() == EJoinType::Inner ? nullptr : join.Filter());
        const auto crossGroup = StageLabel("cross-join", crossStageId);
        TKernelCompiler compiler(
            KernelOptions(crossGroup, crossStageId));
        auto crossKernels = std::make_shared<TCrossJoinKernels>(
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, crossGroup);
                return compiler.CompileCrossJoin(kernelSpec);
            }());
        auto crossCode = MakeBinaryJoinCode<TSchedulerCrossJoinState>();

        NScheduler::IConnection* crossOut = &outConn;
        NScheduler::IConnection* residualConn = nullptr;
        if (filtersProduct) {
            residualConn = &AddConn<NScheduler::TOneToOneConnection>(
                lanes, lanes, "cross-residual-input");
            crossOut = residualConn;
        }

        std::vector<NScheduler::TTaskNode*> crossNodes;
        crossNodes.reserve(lanes);
        for (size_t m = 0; m < lanes; ++m) {
            auto task = std::make_unique<NScheduler::TBinaryBlockingTask>(
                crossCode,
                std::make_shared<TSchedulerCrossJoinState>(
                    *crossKernels, join.JoinType()),
                NScheduler::TInputPort{.Connection = vectorRef, .Lane = m},
                NScheduler::TInputPort{.Connection = scalarInput, .Lane = m},
                NScheduler::TOutputPort{.Connection = crossOut, .Lane = m});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "join",
                crossStageId,
                JoinDebugLabel(join),
                crossStageId);
            if (gatherVector) {
                for (size_t s = 0; s < leftLanes; ++s) {
                    Graph_.AddEdge(*vectorOut.Producers[s], node, *vectorRef, s, m);
                }
            } else {
                Graph_.AddEdge(*vectorOut.Producers[m], node, *vectorRef, m, m);
            }
            if (directScalar) {
                Graph_.AddEdge(
                    *scalarOut.Producers[0], node, *scalarInput, 0, 0, 1);
            } else {
                Graph_.AddEdge(
                    *scalarBroadcastProducer, node, *scalarInput, 0, m, 1);
            }
            crossNodes.push_back(&node);
        }

        if (!filtersProduct) {
            return TLoweredOutput{
                .Producers = std::move(crossNodes),
                .OutputType = std::move(crossType),
            };
        }

        // Residual predicate is a plain filter over the glued schema.
        auto filterOp =
            std::make_shared<TFilterOperator>(join.Left(), join.Filter());
        // The synthetic filter consumes the cross output, including both
        // inputs. Its constructor inherits only the left input's schema.
        filterOp->Type = std::make_shared<NQumir::NAst::TFunctionType>(
            std::vector<NQumir::NAst::TTypePtr>{crossType}, crossType);
        const auto residualStageId = NewExecStage(
            &join, EExecPlanNodeKind::CrossResidualFilter);
        const auto residualGroup = StageLabel(
            "cross-residual-filter", residualStageId);
        TSchedulerUnaryStage stage =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, residualGroup);
                // The residual is a separate executable stage even though its
                // semantic metadata points at the owning join operator.
                return BuildSchedulerFilterStage(
                    *filterOp, crossType,
                    KernelOptions(residualGroup, residualStageId));
            }();
        TLoweredOutput result;
        result.OutputType = std::move(stage.OutputType);
        result.Producers.reserve(lanes);
        for (size_t m = 0; m < lanes; ++m) {
            auto task = std::make_unique<NScheduler::TUnaryTask>(
                stage.Code,
                stage.MakeState(m),
                NScheduler::TInputPort{.Connection = residualConn, .Lane = m},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = m + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "filter",
                residualStageId,
                "filter",
                residualStageId);
            Graph_.AddEdge(*crossNodes[m], node, *residualConn, m, m);
            result.Producers.push_back(&node);
        }
        return result;
    }

    TLoweredOutput LowerJoin(
        TJoinOperator& join,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        using namespace NQumir::NAst;
        const size_t leftLanes = OutputLanes(join.Left());
        const size_t rightLanes = OutputLanes(join.Right());
        const size_t joinParts = JoinPartitions(std::max(leftLanes, rightLanes));
        auto& leftPipeRef = AddConn<NScheduler::TOneToOneConnection>(
            leftLanes, leftLanes, "join-left-input");
        auto leftOut = Lower(join.Left(), leftPipeRef);

        auto& rightPipeRef = AddConn<NScheduler::TOneToOneConnection>(
            rightLanes, rightLanes, "join-right-input");
        auto rightOut = Lower(join.Right(), rightPipeRef);

        auto leftType = leftOut.OutputType;
        auto rightType = rightOut.OutputType;
        auto* leftStruct = static_cast<TStructType*>(leftType.get());
        auto* rightStruct = static_cast<TStructType*>(rightType.get());
        if (!leftStruct || !rightStruct) {
            throw std::runtime_error("scheduler join inputs must have TStructType");
        }

        auto kernelSpec = NKernel::BuildJoinKernelSpec(
            *leftStruct, *rightStruct, join.Keys(), join.JoinType(), join.Filter());
        const auto execStageId = NewExecStage(
            &join, EExecPlanNodeKind::Join);
        const auto joinGroup = StageLabel("join", execStageId);
        TKernelCompiler compiler(
            KernelOptions(joinGroup, execStageId));
        const auto buildSide = ChooseJoinBuildSide(join, Settings_.Join.Table);
        const bool rowTable = UsesRowJoinTable(join, buildSide);
        auto joinCode = rowTable
            ? MakeBinaryJoinCode<TSchedulerRowJoinState>()
            : MakeBinaryJoinCode<TSchedulerInnerJoinState>();
        std::shared_ptr<TJoinKernels> joinKernels;
        std::shared_ptr<TRowJoinKernels> rowJoinKernels;
        auto compileJoinKernels = [&]() {
            TStageDiagnosticsScope diagnosticsScope(Diagnostics_, joinGroup);
            if (rowTable) {
                rowJoinKernels = std::make_shared<TRowJoinKernels>(
                    compiler.CompileRowJoin(kernelSpec, buildSide == EJoinBuildSide::Left));
            } else {
                joinKernels = std::make_shared<TJoinKernels>(compiler.CompileJoin(kernelSpec));
            }
        };
        auto makeJoinState = [&]() -> std::shared_ptr<NScheduler::ITaskState> {
            if (rowTable) {
                return std::make_shared<TSchedulerRowJoinState>(
                    *rowJoinKernels, buildSide, Settings_.Join.RowJoinProbeHoldBytes);
            }
            return std::make_shared<TSchedulerInnerJoinState>(
                *joinKernels, join.JoinType(), buildSide);
        };

        // A filtered join still needs the hash-shuffle tasks to publish and
        // apply the filter, even when both inputs have only one lane.
        if (leftLanes == 1 && rightLanes == 1 && joinParts == 1
            && !join.RuntimeFilter()) {
            compileJoinKernels();
            auto task = std::make_unique<NScheduler::TBinaryBlockingTask>(
                joinCode,
                makeJoinState(),
                NScheduler::TInputPort{
                    .Connection = &leftPipeRef, .Lane = 0},
                NScheduler::TInputPort{
                    .Connection = &rightPipeRef, .Lane = 0},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = 0 + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "join",
                execStageId,
                JoinDebugLabel(join),
                execStageId);
            Graph_.AddEdge(*leftOut.Producers[0], node, leftPipeRef, 0, 0);
            Graph_.AddEdge(
                *rightOut.Producers[0], node, rightPipeRef, 0, 0, 1);
            return TLoweredOutput{
                .Producers = {&node},
                .OutputType = kernelSpec.OutputSchema,
            };
        }

        auto hashKernels = compiler.CompileJoinHash(kernelSpec);
        kernelSpec.HasPrecomputedHash = true;
        compileJoinKernels();
        std::vector<std::string> leftPartitionKeys;
        std::vector<std::string> rightPartitionKeys;
        leftPartitionKeys.reserve(join.Keys().size());
        rightPartitionKeys.reserve(join.Keys().size());
        for (const auto& key : join.Keys()) {
            leftPartitionKeys.push_back(key.Left);
            rightPartitionKeys.push_back(key.Right);
        }
        auto leftShuffleCode = MakeHashShuffleCode(hashKernels.Left, leftType);
        auto rightShuffleCode = MakeHashShuffleCode(hashKernels.Right, rightType);
        bool filterBuildIsLeft = false;
        std::shared_ptr<IRuntimeFilterProbe> filterProbe;
        if (const auto& runtimeFilter = join.RuntimeFilter()) {
            if (!RuntimeContext_->RuntimeFilterKernels) {
                RuntimeContext_->RuntimeFilterKernels = compiler.CompileRuntimeFilter();
            }
            if (!RuntimeFilterBindings_) {
                RuntimeFilterBindings_ =
                    std::make_shared<TLocalRuntimeFilterBindingFactory>();
            }
            const bool buildIsLeft =
                runtimeFilter->BuildSide == EJoinFilterSide::Left;
            filterBuildIsLeft = buildIsLeft;
            // Every build repartition lane contributes before publication.
            // Probe tasks wait for the complete filter.
            const size_t producerCount = buildIsLeft ? leftLanes : rightLanes;
            auto binding = RuntimeFilterBindings_->Create(runtimeFilter->Id,
                producerCount, ChooseRuntimeBloomBytes(
                    join, runtimeFilter->BuildSide, producerCount),
                RuntimeContext_);
            if (!binding.Producer || !binding.Probe) {
                throw std::runtime_error(
                    "runtime filter binding factory returned an incomplete binding");
            }
            (buildIsLeft ? leftShuffleCode : rightShuffleCode)->ProducedFilter =
                std::move(binding.Producer);
            filterProbe = std::move(binding.Probe);
            // TRowSet::Hash has no key identity. Another consumer on this
            // path may leave a hash for a different join key, and a nested
            // join may produce its own hash-bearing rowsets. Recompute in
            // those cases before partitioning this join.
            (buildIsLeft ? rightShuffleCode : leftShuffleCode)
                ->UseInputHash = CanReuseRuntimeFilterHash(
                    buildIsLeft ? join.Right() : join.Left(), runtimeFilter->Id);
        }
        auto leftShuf = BuildRepartitionExchange(
            leftOut,
            leftPipeRef,
            leftLanes,
            joinParts,
            std::move(leftShuffleCode),
            "join-left-repartition",
            {.Keys = std::move(leftPartitionKeys)});
        auto rightShuf = BuildRepartitionExchange(
            rightOut,
            rightPipeRef,
            rightLanes,
            joinParts,
            std::move(rightShuffleCode),
            "join-right-repartition",
            {.Keys = std::move(rightPartitionKeys)});

        if (join.RuntimeFilter()) {
            const auto& buildNodes = filterBuildIsLeft
                ? leftShuf.Nodes : rightShuf.Nodes;
            FilterProducers_.push_back(TFilterProducerTasks{
                .Id = join.RuntimeFilter()->Id,
                .KernelSpec = kernelSpec,
                .HashKernels = hashKernels,
                .Probe = std::move(filterProbe),
                .Nodes = buildNodes,
                .BuildSide = join.RuntimeFilter()->BuildSide,
                .ExecStageId = execStageId,
                .Group = joinGroup,
            });
        }

        TLoweredOutput result;
        result.OutputType = kernelSpec.OutputSchema;
        result.Producers.reserve(joinParts);
        for (size_t j = 0; j < joinParts; ++j) {
            auto task = std::make_unique<NScheduler::TBinaryBlockingTask>(
                joinCode,
                makeJoinState(),
                NScheduler::TInputPort{
                    .Connection = leftShuf.Connection, .Lane = j},
                NScheduler::TInputPort{
                    .Connection = rightShuf.Connection, .Lane = j},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = j + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "join",
                execStageId,
                JoinDebugLabel(join),
                execStageId);
            for (size_t s = 0; s < leftLanes; ++s) {
                Graph_.AddEdge(*leftShuf.Nodes[s], node, *leftShuf.Connection, s, j);
            }
            for (size_t s = 0; s < rightLanes; ++s) {
                Graph_.AddEdge(
                    *rightShuf.Nodes[s], node, *rightShuf.Connection, s, j, 1);
            }
            result.Producers.push_back(&node);
        }
        return result;
    }

    TBlockingTail BuildWindowTail(
        const NQumir::NAst::TTypePtr& childType,
        TWindowOperator& window,
        std::string stage,
        TExecStageId execStageId)
    {
        TKernelCompilerOptions options =
            KernelOptions(std::move(stage), execStageId);
        auto runtime = BuildWindowRuntimeProcess(window, childType, std::move(options));
        return TBlockingTail{
            .Code = MakeDrainBlockingCode<TWindowBlockingState>(),
            .MakeState = [runtime]() -> std::shared_ptr<NScheduler::ITaskState> {
                return std::make_shared<TWindowBlockingState>(
                    runtime.OutputType,
                    runtime.Keys,
                    runtime.KeyColumns,
                    runtime.Kernel);
            },
            .OutputType = runtime.OutputType,
        };
    }

    TLoweredOutput LowerWindow(
        TWindowOperator& window,
        NScheduler::IConnection& outConn,
        size_t outLaneOffset)
    {
        const size_t childLanes = OutputLanes(window.Input());
        const auto execStageId = NewExecStage(
            &window, EExecPlanNodeKind::Window);
        const auto windowGroup = StageLabel("window", execStageId);

        if (childLanes <= 1 || window.PartitionKeys().empty()) {
            return LowerBlocking(
                window.Input(),
                "window",
                windowGroup,
                execStageId,
                [&, windowGroup](const NQumir::NAst::TTypePtr& childType) {
                    return BuildWindowTail(
                        childType, window, windowGroup, execStageId);
                },
                outConn,
                outLaneOffset);
        }

        const size_t parts = JoinPartitions(childLanes);
        auto& childConnRef = AddConn<NScheduler::TOneToOneConnection>(
            childLanes, childLanes, "window-shuffle-input");
        auto childOut = Lower(window.Input(), childConnRef);

        auto childType = childOut.OutputType;
        auto* childStruct =
            static_cast<NQumir::NAst::TStructType*>(childType.get());
        if (!childStruct) {
            throw std::runtime_error("window input must have TStructType");
        }

        TKernelCompiler hashCompiler(KernelOptions(windowGroup, execStageId));
        auto partitionHash = hashCompiler.CompileAggregateHash(
            *childStruct, window.PartitionKeys());
        auto hashCode = MakeHashShuffleCode(std::move(partitionHash), childType);
        auto shuf = BuildRepartitionExchange(
            childOut,
            childConnRef,
            childLanes,
            parts,
            std::move(hashCode),
            "window-repartition",
            {.Keys = window.PartitionKeys()});

        TBlockingTail tail =
            [&]() {
                TStageDiagnosticsScope diagnosticsScope(Diagnostics_, windowGroup);
                return BuildWindowTail(
                    childType, window, windowGroup, execStageId);
            }();

        TLoweredOutput result;
        result.OutputType = tail.OutputType;
        result.Producers.reserve(parts);
        for (size_t m = 0; m < parts; ++m) {
            auto task = std::make_unique<NScheduler::TBlockingTask>(
                tail.Code,
                tail.MakeState(),
                NScheduler::TInputPort{.Connection = shuf.Connection, .Lane = m},
                NScheduler::TOutputPort{.Connection = &outConn, .Lane = m + outLaneOffset});
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "window",
                execStageId,
                "window",
                execStageId);
            for (size_t s = 0; s < childLanes; ++s) {
                Graph_.AddEdge(*shuf.Nodes[s], node, *shuf.Connection, s, m);
            }
            result.Producers.push_back(&node);
        }
        return result;
    }

    TRepartitionExchange BuildRepartitionExchange(
        const TLoweredOutput& input,
        NScheduler::IConnection& inputConn,
        size_t inputLanes,
        size_t partitions,
        std::shared_ptr<NScheduler::THashShuffleCode> hashCode,
        std::string debugName,
        NScheduler::TRepartitionSpec repartition)
    {
        const std::string label = debugName;
        auto* shufPtr = &AddConn<NScheduler::THashShuffleConnection>(
            inputLanes,
            partitions,
            std::move(debugName),
            ShuffleQueueCapacity(),
            std::move(repartition));

        TRepartitionExchange side;
        side.Connection = shufPtr;
        side.Nodes.reserve(inputLanes);
        const auto shuffleTaskGroupId = NewTaskGroup();
        for (size_t p = 0; p < inputLanes; ++p) {
            auto task = std::make_unique<NScheduler::THashShuffleTask>(
                hashCode,
                std::make_shared<int>(0),
                NScheduler::TInputPort{.Connection = &inputConn, .Lane = p},
                *shufPtr,
                p);
            auto& node = Graph_.AddOwnedNode(std::move(task));
            MarkNode(node,
                "hash-shuffle",
                shuffleTaskGroupId,
                label);
            Graph_.AddEdge(*input.Producers[p], node, inputConn, p, p);
            side.Nodes.push_back(&node);
        }
        return side;
    }

    std::shared_ptr<NScheduler::THashShuffleCode> MakeHashShuffleCode(
        NScheduler::THashShuffleCode::THash hash,
        NQumir::NAst::TTypePtr inputType) const
    {
        return std::make_shared<NScheduler::THashShuffleCode>(
            std::move(hash),
            std::move(inputType),
            Settings_.HashShuffle.TargetOutputBatchRows,
            Settings_.HashShuffle.MaxOutputBatchRows,
            Settings_.HashShuffle.TargetOutputBatchBytes);
    }

public:
    std::vector<TLoweredExecStage>& ExecStages() {
        return ExecStages_;
    }
    std::vector<NScheduler::TRuntimeFilterDiagnostic>& RuntimeFilters() {
        return RuntimeFilters_;
    }

private:
    TExecStageId NewExecStage(
        const IOperator* op,
        EExecPlanNodeKind kind,
        TOperatorPtr ownedOp = {})
    {
        const auto id = static_cast<TExecStageId>(ExecStages_.size() + 1);
        ExecStages_.push_back({
            .Id = id,
            .Kind = std::move(kind),
            .Operator = op,
            .OperatorOwner = std::move(ownedOp),
        });
        return id;
    }

    TExecStageId NewAggregateStage(
        const IOperator* op,
        EAggregatePhase phase,
        TOperatorPtr ownedOp = {})
    {
        const auto id = NewExecStage(
            op, EExecPlanNodeKind::Aggregate, std::move(ownedOp));
        ExecStages_.back().AggregatePhase = phase;
        return id;
    }

    NScheduler::TTaskGroupId NewTaskGroup() {
        return ++LastTaskGroupId_;
    }

    NScheduler::TTaskGraph& Graph_;
    NScheduler::TSettings Settings_;
    std::ostream* Diagnostics_;
    std::vector<TGeneratedKernel>* KernelSink_ = nullptr;
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog_;
    std::shared_ptr<IRuntimeFilterBindingFactory> RuntimeFilterBindings_;
    std::shared_ptr<TRuntimeContext> RuntimeContext_;
    std::unordered_set<const TSourceOperator*> SourcesFeedingJoins_;
    // Split planning includes Parquet predicate compilation and evaluation;
    // OutputLanes and LowerSource both query it. nullopt means "use the original
    // source", while an engaged empty vector is a proven empty scan.
    mutable std::unordered_map<const TSourceOperator*, TSourceSplits>
        ScanSplitCache_;
    std::unordered_map<uint32_t, std::vector<TFilterConsumerTasks>> FilterConsumers_;
    std::vector<TFilterProducerTasks> FilterProducers_;
    std::unordered_map<const TCteMaterialization*, TMaterializedProducer> Materialized_;
    std::vector<TLoweredExecStage> ExecStages_;
    std::vector<NScheduler::TRuntimeFilterDiagnostic> RuntimeFilters_;
    NScheduler::TTaskGroupId LastTaskGroupId_ =
        NScheduler::InvalidTaskGroupId;
};

} // namespace

namespace NScheduler {

bool ShouldUsePartialAggregate(
    const TAggregateOperator& aggregate,
    const TSettings& settings)
{
    if (!settings.Aggregate.EnablePartial ||
        aggregate.GroupKeys().empty() ||
        !aggregate.GroupingSets().empty())
    {
        return false;
    }

    const auto inputStats = aggregate.Input()->Stats_;
    const auto estimatedGroups = EstimateAggregateGroupCount(aggregate);
    if (!inputStats || !estimatedGroups || *estimatedGroups == 0 ||
        inputStats->RowCount < settings.Aggregate.PartialMinInputRows)
    {
        return false;
    }

    const uint64_t reduction = std::max<size_t>(
        settings.Aggregate.PartialMinReductionFactor, 1);
    return *estimatedGroups <= inputStats->RowCount / reduction;
}

size_t ChooseAggregatePartitionCount(
    const TAggregateOperator& aggregate,
    size_t inputLanes,
    const TSettings& settings)
{
    const size_t workers = std::max<size_t>(settings.Scheduler.WorkerCount, 1);
    const size_t base = std::min(inputLanes, std::max<size_t>(workers / 2, 1));

    // An explicit --shuffle-partitions is an exact user override, matching the
    // existing join behavior.
    if (settings.HashShuffle.PartitionCount != 0) {
        const size_t maxPartitions = settings.HashShuffle.MaxPartitionCount == 0
            ? settings.HashShuffle.PartitionCount
            : settings.HashShuffle.MaxPartitionCount;
        return std::max<size_t>(
            std::min(settings.HashShuffle.PartitionCount, maxPartitions),
            1);
    }

    auto result = std::max<size_t>(base, 1);
    const auto inputStats = aggregate.Input()->Stats_;
    const auto estimatedGroups = EstimateAggregateGroupCount(aggregate);
    if (!inputStats || !estimatedGroups || *estimatedGroups == 0 ||
        !aggregate.Input()->Type)
    {
        return result;
    }

    auto maybeInputStruct = NQumir::NAst::TMaybeType<
        NQumir::NAst::TStructType>(NQumir::NAst::UnwrapNamedType(
            aggregate.Input()->OutputColumns()));
    if (!maybeInputStruct) {
        return result;
    }
    const auto inputStruct = maybeInputStruct.Cast().get();

    // String shuffle copies payload bytes and showed mixed results at full
    // fanout. Start with the statistically predictable fixed-width case.
    for (const auto& key : aggregate.GroupKeys()) {
        auto type = FieldType(inputStruct, key);
        if (!type) {
            return result;
        }
        const bool isDecimal = IsDecimalType(
            UnwrapNullableType(type));
        type = NQumir::NAst::UnwrapNamedType(
            UnwrapNullableType(type));
        const bool fixedWidth =
            static_cast<bool>(NQumir::NAst::TMaybeType<
                NQumir::NAst::TIntegerType>(type)) ||
            static_cast<bool>(NQumir::NAst::TMaybeType<
                NQumir::NAst::TFloatType>(type)) ||
            static_cast<bool>(NQumir::NAst::TMaybeType<
                NQumir::NAst::TBoolType>(type)) ||
            isDecimal;
        if (!fixedWidth) {
            return result;
        }
    }

    constexpr uint64_t TargetGroupsPerPartition = 1ull << 20;
    // For smaller/filtered inputs the extra scheduler and hash-shuffle fanout
    // outweighed the added aggregate parallelism in ClickBench. Require at
    // least two target-sized chunks of input per worker before widening.
    if (inputStats->RowCount / TargetGroupsPerPartition < 2 * workers) {
        return result;
    }

    const uint64_t desiredFromGroups =
        *estimatedGroups / TargetGroupsPerPartition +
        (*estimatedGroups % TargetGroupsPerPartition != 0);
    const size_t maxPartitions = std::min(inputLanes, workers);
    result = std::min<size_t>(
        std::max<uint64_t>(desiredFromGroups, result),
        maxPartitions);
    if (settings.HashShuffle.MaxPartitionCount != 0) {
        result = std::min(result, settings.HashShuffle.MaxPartitionCount);
    }
    return std::max<size_t>(result, 1);
}

namespace {

std::shared_ptr<TSinkCode> MakeSinkWriterCode() {
    return std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            static_cast<ISink*>(state)->Write(rowSet);
        });
}

const char* AggregateTableKindName(EAggregateTableKind table) {
    switch (table) {
        case EAggregateTableKind::Auto:
            return "auto";
        case EAggregateTableKind::Swiss:
            return "swiss";
        case EAggregateTableKind::Row:
            return "row";
    }
    return "unknown";
}

void PrintGraphDiagnostics(
    std::ostream& out, const TTaskGraph& graph, const TSettings& settings)
{
    out << "\n========== SCHEDULER GRAPH ==========\n";
    out << "mode=" << static_cast<int>(settings.Scheduler.Mode)
        << " workers=" << settings.Scheduler.WorkerCount
        << " ready_queue=" << settings.Scheduler.ReadyQueueCapacity
        << " rowset_queue=" << settings.Queue.RowsetCapacityPerLane
        << " scan_tasks=";
    if (settings.ScanSplit.MaxScanTasks == 0) {
        out << "auto";
    } else {
        out << settings.ScanSplit.MaxScanTasks;
    }
    if (settings.HashShuffle.PartitionCount == 0) {
        out << " shuffle_parts=auto";
    } else {
        out << " shuffle_parts=" << settings.HashShuffle.PartitionCount;
    }
    out << " shuffle_queue=" << settings.HashShuffle.MaxQueuedRowsetsPerLane
        << " shuffle_target_rows=" << settings.HashShuffle.TargetOutputBatchRows
        << " shuffle_max_rows=" << settings.HashShuffle.MaxOutputBatchRows
        << " shuffle_target_bytes=" << settings.HashShuffle.TargetOutputBatchBytes
        << " partial_aggregate="
        << (settings.Aggregate.EnablePartial ? "auto" : "off")
        << " aggregate_table=" << AggregateTableKindName(settings.Aggregate.Table)
        << "\n";
    graph.Print(out);
    out << "=====================================\n";
}

void PrintCounterDiagnostics(
    std::ostream& out, const TTaskGraph& graph, const TSchedulerRunStats& stats)
{
    out << "\n========== SCHEDULER COUNTERS ==========\n"
        << "scheduled=" << stats.Scheduled
        << " popped=" << stats.Popped
        << " executed=" << stats.Executed
        << " ok=" << stats.Ok
        << " need_data=" << stats.NeedData
        << " blocked_output=" << stats.BlockedOutput
        << " finished=" << stats.Finished
        << " rescheduled=" << stats.Rescheduled
        << " ready_push_retries=" << stats.ReadyPushRetries
        << " empty_ready_polls=" << stats.EmptyReadyPolls
        << "\n";
    graph.PrintConnectionStats(out);
    out << "========================================\n";
}

} // namespace

TLoweredPlan LowerPlanToGraph(
    const TOperatorPtr& root,
    TSettings settings,
    std::ostream* diagnostics,
    std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog,
    std::shared_ptr<IRuntimeFilterBindingFactory> filterBindings)
{
    PlaceRuntimeFilterConsumers(root);
    auto graph = std::make_unique<TTaskGraph>();
    std::vector<TGeneratedKernel> kernels;
    TSchedulerGraphLowerer lowerer(
        *graph, root, settings, diagnostics, &kernels, externalCatalog,
        std::move(filterBindings));
    const size_t lanes = lowerer.OutputLanes(root);
    if (lanes == 0) {
        throw std::runtime_error(
            "scheduler lowering produced no output lanes for the plan");
    }

    IConnection* finalRef = nullptr;
    if (lanes == 1) {
        auto conn = std::make_unique<TOneToOneConnection>(lowerer.QueueCapacity());
        conn->Resize(1, 1);
        conn->SetDebugName("final-output");
        finalRef = &graph->AddConnection(std::move(conn));
    } else {
        auto gather = std::make_unique<TGatherConnection>(lowerer.QueueCapacity());
        gather->Resize(lanes, 1);
        gather->SetDebugName("final-gather");
        finalRef = &graph->AddConnection(std::move(gather));
    }
    auto out = lowerer.Lower(root, *finalRef);
    lowerer.CheckMaterializationsWired();
    lowerer.WireRuntimeFilters();
    lowerer.CheckExecStagesConsistent();

    return TLoweredPlan{
        .Graph = std::move(graph),
        .OutputType = std::move(out.OutputType),
        .FinalGather = finalRef,
        .Producers = std::move(out.Producers),
        .Lanes = lanes,
        .Kernels = std::move(kernels),
        .ExternalCatalog = std::move(externalCatalog),
        .ExecStages = std::move(lowerer.ExecStages()),
        .RuntimeFilters = std::move(lowerer.RuntimeFilters()),
    };
}

bool RunPlanIntoSink(
    TLoweredPlan lowered,
    ISink& sink,
    TSettings settings,
    std::ostream* diagnostics,
    std::string* error)
{
    // Aliasing shared_ptr: the sink task's state points at the caller-owned
    // sink without taking ownership of it.
    std::shared_ptr<void> sinkState(std::shared_ptr<void>{}, &sink);
    auto sinkTask = std::make_unique<TSinkTask>(
        MakeSinkWriterCode(),
        std::move(sinkState),
        TInputPort{.Connection = lowered.FinalGather, .Lane = 0});
    auto& sinkNode = lowered.Graph->AddOwnedNode(std::move(sinkTask));
    sinkNode.DebugKind = "sink";
    for (size_t p = 0; p < lowered.Lanes; ++p) {
        lowered.Graph->AddEdge(
            *lowered.Producers[p], sinkNode, *lowered.FinalGather, p, 0);
    }

    lowered.Graph->Build();
    if (settings.Instrumentation.Operators) {
        EnableOperatorInstrumentation(lowered);
    }
    JitFinalizeKernels(lowered.Kernels, diagnostics, lowered.ExternalCatalog);
    if (!lowered.Graph->Validate(error)) {
        return false;
    }
    lowered.Graph->SetConnectionStatsEnabled(settings.Queue.EnableDebugCounters);
    if (diagnostics) {
        PrintGraphDiagnostics(*diagnostics, *lowered.Graph, settings);
    }

    if (settings.Queue.EnableDebugCounters && diagnostics) {
        for (const auto& diagnostic : lowered.RuntimeFilters) {
            if (auto local = std::dynamic_pointer_cast<TRuntimeFilter>(
                    diagnostic.Probe)) {
                local->StartExecutionClock();
            }
        }
    }

    TSchedulerExecutor executor(*lowered.Graph, settings);
    if (!executor.Run(error)) {
        return false;
    }
    if (settings.Instrumentation.Operators && settings.Instrumentation.Output) {
        PrintOperatorInstrumentation(*settings.Instrumentation.Output, *lowered.Graph);
    }
    if (settings.Queue.EnableDebugCounters && diagnostics) {
        PrintCounterDiagnostics(*diagnostics, *lowered.Graph, executor.Stats());
        for (const auto& diagnostic : lowered.RuntimeFilters) {
            auto local = std::dynamic_pointer_cast<TRuntimeFilter>(diagnostic.Probe);
            if (!local) {
                continue;
            }
            const auto stats = local->StatsSnapshot();
            *diagnostics << "runtime-filter " << diagnostic.Id
                << " at=" << (diagnostic.AtSource ? "source" : "operator")
                << " build_rows=" << stats.BuildRows
                << " probe_rows=" << stats.ProbeRows
                << " rejected=" << stats.RejectedRows
                << " skipped_probe=" << stats.SkippedProbeRows
                << " paused_probe=" << stats.PausedProbeRows
                << " exact_keys=" << stats.ExactKeys
                << " bloom_bytes=" << stats.BloomBytes
                << " bloom_set_bits=" << stats.BloomSetBits
                << " ready_ms=" << stats.ReadyLatencyMs
                << " ready=" << stats.Ready
                << " bloom_disabled=" << stats.BloomDisabled
                << '\n';
        }
    }
    return true;
}

} // namespace NScheduler
} // namespace NQdb
