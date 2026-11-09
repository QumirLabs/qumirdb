#pragma once

#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/late_materialize_exec.h>
#include <qdb/exec/row_join_exec.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/unary_process.h>
#include <qdb/exec/window_exec.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NQdb::NScheduler::NLowering {

// Task state structs and code factories used by the plan lowerer.

template <class TState>
struct TProcessorTaskState : NScheduler::ITaskState {
    void ConfigureInstrumentation(TOperatorInstrumentation* instrumentation) override {
        if constexpr (requires(TState& state) { state.Processor.SetInstrumentation(instrumentation); }) {
            static_cast<TState&>(*this).Processor.SetInstrumentation(instrumentation);
        }
    }
};

// The source a source task reads; OwnedSource is set when the task owns it.
struct TSchedulerSourceState {
    ISource* Source = nullptr;
    std::unique_ptr<ISource> OwnedSource;
};

// Source code that reads TSchedulerSourceState::Source until it ends.
std::shared_ptr<const NScheduler::TSourceCode> MakeSourceCode();

struct TAggregateBlockingState : TProcessorTaskState<TAggregateBlockingState> {
    explicit TAggregateBlockingState(
        TAggregateKernels kernels,
        TAggregateTablePlan plan)
        : Processor(std::move(kernels), plan)
    {}

    TAggregateProcessor Processor;
    bool Done = false;
};

// Blocking code that feeds every input row to TAggregateBlockingState, then
// emits the groups.
std::shared_ptr<const NScheduler::TBlockingCode> MakeAggregateBlockingCode();

struct TPartialAggregateBlockingState : TProcessorTaskState<TPartialAggregateBlockingState> {
    TPartialAggregateBlockingState(
        TAggregateKernels kernels,
        TAggregateTablePlan plan,
        TPartialAggregatePolicy policy,
        TUnaryStreamProcess passThrough)
        : Processor(std::move(kernels), plan, policy, std::move(passThrough))
    {}

    TPartialAggregateProcessor Processor;
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

struct TBlockingTail {
    std::shared_ptr<const NScheduler::TBlockingCode> Code;
    std::function<std::shared_ptr<NScheduler::ITaskState>()> MakeState;
    NQumir::NAst::TTypePtr OutputType;
    // Appended to the label of the tasks, e.g. an aggregate's table.
    std::string LabelDetail;
};

EJoinFetchResult MapJoinFetch(NScheduler::EFetchResult result);
NScheduler::ETaskResult MapJoinProcessResult(EJoinProcessorResult result);

// Blocking code for a processor with Add/Next/InputFinished methods. It
// drains the input first, then emits processor output.
template <class TState>
std::shared_ptr<const NScheduler::TBlockingCode> MakeDrainBlockingCode()
{
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

std::shared_ptr<const NScheduler::TBlockingCode> MakeLimitBlockingCode();

// Merge code for TLateMaterializeMergeState: joins the column slices of the
// lookups once each has produced one.
std::shared_ptr<const NScheduler::TMergeCode> MakeLateMergeCode();

// Merge code for TMergeState: reads every sorted run, then emits the merge.
std::shared_ptr<const NScheduler::TMergeCode> MakeMergeCode();

// Binary blocking code adapting scheduler input ports to a join/cross
// processor's fetch-callback interface. TState wraps the processor.
template <class TState>
std::shared_ptr<const NScheduler::TBinaryBlockingCode> MakeBinaryJoinCode()
{
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

} // namespace NQdb::NScheduler::NLowering
