#pragma once

#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/instrumentation/operator.h>
#include <qdb/io/io.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/state.h>
#include <qdb/scheduler/task_state.h>

#include <qumir/parser/type.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace NQdb {
namespace NScheduler {

struct TTaskNode;

enum class ERequiredSide { Both = 0, Left = 1, Right = 2 };

struct TInputPort {
    IConnection* Connection = nullptr;
    size_t Lane = 0;
    TRowSetInstrumentation* Instrumentation = nullptr;

    EFetchResult Fetch(TRowSet& rowSet) const;
};

struct TOutputPort {
    IConnection* Connection = nullptr;
    size_t Lane = 0;
    TRowSetInstrumentation* Instrumentation = nullptr;

    bool CanPush() const;
    bool Push(TRowSet&& rowSet) const;
    void Finish() const;
};

enum class EAuxInputStatus { Ready, NeedProducer, Waiting, Failed };

// Query-local auxiliary input. It carries filter publication state rather
// than rowsets; dependency edges supply demand and wakeups.
class TAuxInput {
public:
    void AddFilter(std::shared_ptr<IRuntimeFilterProbe> filter);
    void AddProducer(TTaskNode& producer);
    bool Ready() const;
    EAuxInputStatus Poll() const;
    // A waiting consumer wants its idle producers; a ready one, its rowsets.
    bool WantsInput(const TTaskEdge& edge) const noexcept;
    // Whether a producer's progress can change the consumer's state.
    bool Settled() const noexcept;
    // Ready() for a consumer's Execute: throws when every producer finished
    // without publishing.
    bool CheckReady() const;

private:
    std::vector<std::shared_ptr<IRuntimeFilterProbe>> Filters_;
    std::vector<TTaskNode*> Producers_;
};

struct TSourceCode {
    using TNext = std::function<bool(void*, TRowSet&)>;

    explicit TSourceCode(TNext next, std::shared_ptr<TAuxInput> auxInput = nullptr);

    TNext Next;
    // Runtime filters the source applies while it reads. The task reads
    // nothing before they are published.
    std::shared_ptr<TAuxInput> AuxInput;
};

struct TUnaryCode {
    using TProcess = std::function<void(void*, TRowSet&)>;

    explicit TUnaryCode(TProcess process);

    TProcess Process;
};

struct TSinkCode {
    using TWrite = std::function<void(void*, const TRowSet&)>;

    explicit TSinkCode(TWrite write);

    TWrite Write;
};

struct TBlockingCode {
    using TProcess = std::function<ETaskResult(ITaskState&, TInputPort&, TRowSet&)>;

    explicit TBlockingCode(TProcess process);

    TProcess Process;
};

struct TBinaryBlockingCode {
    using TProcess = std::function<ETaskResult(
        ITaskState&,
        TInputPort&,
        TInputPort&,
        TRowSet&)>;
    // Which side Process will read next; empty => both. Consulted by the task's
    // WantsInput to gate readiness.
    using TRequiredSide = std::function<ERequiredSide(ITaskState&)>;

    explicit TBinaryBlockingCode(TProcess process);

    TProcess Process;
    TRequiredSide RequiredSide;
};

struct THashShuffleCode {
    using THash = std::function<bool(TRowSet*, uint64_t*)>;

    explicit THashShuffleCode(THash hash);
    THashShuffleCode(THash hash,
        NQumir::NAst::TTypePtr inputType,
        size_t targetOutputBatchRows,
        size_t maxOutputBatchRows,
        size_t targetOutputBatchBytes);

    THash Hash;
    NQumir::NAst::TTypePtr InputType;
    size_t TargetOutputBatchRows = 0;
    size_t MaxOutputBatchRows = 0;
    // Also bounds the full visible input payload referenced by each buffer.
    // Shared references are charged conservatively; one input may exceed it.
    size_t TargetOutputBatchBytes = 0;
    // Build lanes share a producer binding; each publishes one partial.
    std::shared_ptr<IRuntimeFilterProducer> ProducedFilter;
    // Reuse hashes produced by a preceding runtime-filter task for this key.
    bool UseInputHash = false;
};

struct TRuntimeFilterCode {
    THashShuffleCode::THash Hash;
    std::shared_ptr<IRuntimeFilterProbe> Probe;
    std::shared_ptr<TAuxInput> AuxInput;
};

// Probes the keys of rowSet against the filter; state is created on first use,
// after publication. nullopt when the filter does not test this batch
// (unpublished, PassAll or paused): every row passes and no hashes are
// computed. Otherwise fills hashes and selection,
// the input selection narrowed to rows the filter may contain, and returns
// the number of those rows.
std::optional<size_t> ProbeRuntimeFilter(
    const TRuntimeFilterCode& code,
    std::shared_ptr<TRuntimeFilterProbeState>& state,
    TRowSet& rowSet,
    std::vector<uint64_t>& hashes,
    std::vector<uint8_t>& selection);

// The runtime filters placed on a scan, applied by its reader in placement
// order with the result a chain of TRuntimeFilterTask would produce. One
// instance and its probe states belong to one scan task.
class TRuntimeFilterScan final : public IScanFilter {
public:
    TRuntimeFilterScan(
        std::vector<std::shared_ptr<const TRuntimeFilterCode>> codes,
        std::vector<size_t> columns);

    std::span<const size_t> Columns() const override;
    size_t Select(const TRowSet& batch, TScanSelection& selection) override;

private:
    struct TUse {
        std::shared_ptr<const TRuntimeFilterCode> Code;
        std::shared_ptr<TRuntimeFilterProbeState> State;
    };

    std::vector<TUse> Uses_;
    std::vector<size_t> Columns_;
};

struct TMergeCode {
    using TProcess = std::function<ETaskResult(
        ITaskState&,
        std::vector<TInputPort>&,
        TRowSet&)>;

    explicit TMergeCode(TProcess process);

    TProcess Process;
};

class TSourceTask final : public ITaskNode {
public:
    TSourceTask(
        std::shared_ptr<const TSourceCode> code,
        std::shared_ptr<void> state,
        TOutputPort output);

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool WantsInput(const TTaskEdge& edge) const noexcept override;
    bool TryNotifyDependency() noexcept override;

private:
    std::shared_ptr<const TSourceCode> Code_;
    std::shared_ptr<void> State_;
    TOutputPort Output_;
    bool Finished_ = false;
    std::atomic<bool> DependencyNotified_ = false;
};

class TRuntimeFilterTask final : public ITaskNode {
public:
    TRuntimeFilterTask(
        std::shared_ptr<const TRuntimeFilterCode> code,
        TInputPort input,
        TOutputPort output);
    ~TRuntimeFilterTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool WantsInput(const TTaskEdge& edge) const noexcept override;
    bool TryNotifyDependency() noexcept override;

private:
    std::shared_ptr<const TRuntimeFilterCode> Code_;
    std::shared_ptr<TRuntimeFilterProbeState> ProbeState_;
    TInputPort Input_;
    TOutputPort Output_;
    TRowSet CurrentInput_ = {};
    bool HasInput_ = false;
    bool Prepared_ = false;
    bool InputFinished_ = false;
    bool OutputFinished_ = false;
    std::atomic<bool> DependencyNotified_ = false;
};

class TUnaryTask final : public ITaskNode {
public:
    TUnaryTask(
        std::shared_ptr<const TUnaryCode> code,
        std::shared_ptr<void> state,
        TInputPort input,
        TOutputPort output);
    ~TUnaryTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;

    const std::shared_ptr<const TUnaryCode>& Code() const;
    const std::shared_ptr<void>& State() const;

private:
    std::shared_ptr<const TUnaryCode> Code_;
    std::shared_ptr<void> State_;
    TInputPort Input_;
    TOutputPort Output_;
    TRowSet CurrentInput_ = {};
    bool HasInput_ = false;
    bool InputFinished_ = false;
    bool OutputFinished_ = false;
};

class TSinkTask final : public ITaskNode {
public:
    TSinkTask(
        std::shared_ptr<const TSinkCode> code,
        std::shared_ptr<void> state,
        TInputPort input);

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;

private:
    std::shared_ptr<const TSinkCode> Code_;
    std::shared_ptr<void> State_;
    TInputPort Input_;
    bool Finished_ = false;
};

class TBlockingTask final : public ITaskNode {
public:
    TBlockingTask(
        std::shared_ptr<const TBlockingCode> code,
        std::shared_ptr<ITaskState> state,
        TInputPort input,
        TOutputPort output);
    ~TBlockingTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;

private:
    std::shared_ptr<const TBlockingCode> Code_;
    std::shared_ptr<ITaskState> State_;
    TInputPort Input_;
    TOutputPort Output_;
    TRowSet CurrentOutput_ = {};
    bool HasOutput_ = false;
    bool OutputFinished_ = false;
};

class TBinaryBlockingTask final : public ITaskNode {
public:
    TBinaryBlockingTask(
        std::shared_ptr<const TBinaryBlockingCode> code,
        std::shared_ptr<ITaskState> state,
        TInputPort left,
        TInputPort right,
        TOutputPort output);
    ~TBinaryBlockingTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool WantsInput(const TTaskEdge& edge) const noexcept override;

private:
    std::shared_ptr<const TBinaryBlockingCode> Code_;
    std::shared_ptr<ITaskState> State_;
    TInputPort Left_;
    TInputPort Right_;
    TOutputPort Output_;
    TRowSet CurrentOutput_ = {};
    bool HasOutput_ = false;
    bool OutputFinished_ = false;
};

// N-input blocking task: reads several sorted runs and emits one stream.
class TMergeTask final : public ITaskNode {
public:
    TMergeTask(
        std::shared_ptr<const TMergeCode> code,
        std::shared_ptr<ITaskState> state,
        std::vector<TInputPort> inputs,
        TOutputPort output);
    ~TMergeTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;

private:
    std::shared_ptr<const TMergeCode> Code_;
    std::shared_ptr<ITaskState> State_;
    std::vector<TInputPort> Inputs_;
    TOutputPort Output_;
    TRowSet CurrentOutput_ = {};
    bool HasOutput_ = false;
    bool OutputFinished_ = false;
};

class THashShuffleTask final : public ITaskNode {
public:
    THashShuffleTask(
        std::shared_ptr<const THashShuffleCode> code,
        std::shared_ptr<void> state,
        TInputPort input,
        THashShuffleConnection& output,
        size_t sourceLane);
    ~THashShuffleTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;

private:
    struct TBufferedBatch;
    struct TDestinationBuffer;
    struct TPendingOutput;
    struct TInstrumentationState;

    bool BatchingEnabled() const;
    std::shared_ptr<uint64_t> ComputeHashes(const std::shared_ptr<TRowSet>& input);
    void AccumulateFilter(const TRowSet& rowSet, std::span<const uint64_t> hashes);
    void EnsureBuffers(size_t partitions);
    void Scatter(TRowSet& rowSet);
    void ScatterBuffered(const std::shared_ptr<TRowSet>& input);
    void ScatterViews(const std::shared_ptr<TRowSet>& input);
    void AddBufferedRows(
        size_t dst,
        const std::shared_ptr<TRowSet>& input,
        std::vector<int32_t>&& rows,
        size_t bytes,
        const std::shared_ptr<uint64_t>& hashes,
        size_t inputBytes);
    void FlushBuffer(size_t dst, bool releaseInputs = false);
    void FlushAllBuffers();
    ETaskResult PushPending();
    void ClearPending();
    void ObserveInstrumentation();

private:
    std::shared_ptr<const THashShuffleCode> Code_;
    std::shared_ptr<void> State_;
    TInputPort Input_;
    THashShuffleConnection* Output_ = nullptr;
    size_t SourceLane_ = 0;
    TRowSetInstrumentation* OutputInstrumentation_ = nullptr;
    std::unique_ptr<TInstrumentationState> ResourceInstrumentation_;
    std::optional<TRuntimeFilterBuilder> FilterBuilder_;
    std::vector<std::unique_ptr<TDestinationBuffer>> Buffers_;
    std::vector<TPendingOutput> Pending_;
    size_t PendingIndex_ = 0;
    bool InputFinished_ = false;
    bool OutputFinished_ = false;
};

// Consumers push non-owning views of Batches, so the spool must outlive every
// consumer's downstream (the graph owns both, so it does).
struct TCteSharedState : ITaskState {
    enum class EStatus {
        Building,
        Ready,
    };

    std::atomic<EStatus> Status = EStatus::Building;
    TOperatorInstrumentation* Instrumentation = nullptr;
    std::vector<TRowSet> Batches;

    void ConfigureInstrumentation(TOperatorInstrumentation* instrumentation) override {
        Instrumentation = instrumentation;
    }

    ~TCteSharedState() override {
        for (auto& batch : Batches) {
            Release(&batch);
        }
    }
};

class TCteConsumerTask final : public ITaskNode {
public:
    TCteConsumerTask(std::shared_ptr<const TCteSharedState> state, TOutputPort output);

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;

private:
    std::shared_ptr<const TCteSharedState> State_;
    TOutputPort Output_;
    size_t NextBatch_ = 0;
    bool Finished_ = false;
};

} // namespace NScheduler
} // namespace NQdb
