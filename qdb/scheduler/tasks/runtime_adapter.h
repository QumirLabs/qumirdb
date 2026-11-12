#pragma once

#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/instrumentation/operator.h>
#include <qdb/io/io.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/task.h>
#include <qdb/scheduler/tasks/task_state.h>

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

enum class EAuxInputStatus { Ready, NeedProducer, Waiting, Failed };

// The producers behind the dependency edges into `consumer`: NeedProducer while
// one is idle, Ready once all finished (also when there are none), else Waiting.
EAuxInputStatus PollDependencies(const TTaskNode& consumer);

// Query-local auxiliary input. It carries filter publication state rather
// than rowsets; dependency edges supply demand and wakeups.
class TAuxInput {
public:
    void AddFilter(std::shared_ptr<IRuntimeFilterProbe> filter);
    // The producers are the sources of the consumer's dependency edges.
    void SetConsumer(const TTaskNode& consumer);
    bool Ready() const;
    EAuxInputStatus Poll() const;

private:
    std::vector<std::shared_ptr<IRuntimeFilterProbe>> Filters_;
    const TTaskNode* Consumer_ = nullptr;
};

struct TSourceCode {
    // OK: rowSet is produced. WAITING: no data yet; may be returned again at
    // any time, so Next must tolerate a call before the external wake.
    // FINISHED: no more data.
    using TNext = std::function<ETaskResult(void*, TRowSet&)>;

    explicit TSourceCode(TNext next);

    TNext Next;
};

struct TUnaryCode {
    using TProcess = std::function<void(void*, TRowSet&)>;

    explicit TUnaryCode(TProcess process);

    TProcess Process;
};

struct TSinkCode {
    // OK: written. WAITING: not written, the same rowSet is passed again, maybe
    // before the external wake; Write must be idempotent for a rejected rowSet.
    using TWrite = std::function<ETaskResult(void*, const TRowSet&)>;
    // Optional; called after the last Write. OK: the task finishes. WAITING:
    // called again, maybe before the external wake, so it must be idempotent.
    using TFinish = std::function<ETaskResult(void*)>;

    explicit TSinkCode(TWrite write, TFinish finish = {});

    TWrite Write;
    TFinish Finish;
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

struct TRuntimeFilterSinkCode {
    // Takes the partial; called once.
    using TSend = std::function<void(void*, TRuntimeFilterPartial&&)>;

    explicit TRuntimeFilterSinkCode(TSend send);

    TSend Send;
};

struct TRuntimeFilterSourceCode {
    // True once the partial is there, which it then moves out. May be called
    // before the external wake.
    using TTake = std::function<bool(void*, TRuntimeFilterPartial&)>;

    explicit TRuntimeFilterSourceCode(TTake take);

    TTake Take;
};

struct TMergeCode {
    using TProcess = std::function<ETaskResult(
        ITaskState&,
        std::vector<TInputPort>&,
        TRowSet&)>;

    explicit TMergeCode(TProcess process);

    TProcess Process;
};

class TSourceTask final : public ITask {
public:
    TSourceTask(
        std::shared_ptr<const TSourceCode> code,
        std::shared_ptr<void> state,
        TOutputPort output);

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool RebindPort(const TPortRebind& rebind) override;

    // Only for a task built with empty code (a remote source), before it is scheduled.
    void BindCode(std::shared_ptr<const TSourceCode> code, std::shared_ptr<void> state);
    const std::shared_ptr<const TSourceCode>& Code() const;

private:
    std::shared_ptr<const TSourceCode> Code_;
    std::shared_ptr<void> State_;
    TOutputPort Output_;
    bool Finished_ = false;
};

class TRuntimeFilterTask final : public ITask {
public:
    TRuntimeFilterTask(
        std::shared_ptr<const TRuntimeFilterCode> code,
        TInputPort input,
        TOutputPort output);
    ~TRuntimeFilterTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool RebindPort(const TPortRebind& rebind) override;
    bool WantsInput(const TTaskEdge& edge) const noexcept override;
    bool TryNotifyDependency() noexcept override;

    // Only for a task built with empty code, before it is scheduled.
    void BindCode(std::shared_ptr<const TRuntimeFilterCode> code);
    const IRuntimeFilterProbe* Filter() const;

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

// Sends the merged partial of one filter's build lanes in this graph, the
// producers of its dependency edges, once they all reached their input end:
// their partials are final then, while their output may still drain. If a lane
// is abandoned first, it sends a partial that passes every key.
class TRuntimeFilterSinkTask final : public ITask {
public:
    // Without a filter, a placeholder that never runs.
    explicit TRuntimeFilterSinkTask(std::shared_ptr<const TRuntimeFilterExport> filter = nullptr);

    ETaskResult Execute() override;
    bool WantsInput(const TTaskEdge& edge) const noexcept override;
    bool TryNotifyDependency() noexcept override;

    // Before it is scheduled: the graph node that holds the task.
    void BindNode(const TTaskNode& node);
    // Only for a task built with empty code, before it is scheduled.
    void BindCode(std::shared_ptr<const TRuntimeFilterSinkCode> code, std::shared_ptr<void> state);
    const std::shared_ptr<const TRuntimeFilterSinkCode>& Code() const;

private:
    std::shared_ptr<const TRuntimeFilterExport> Filter_;
    const TTaskNode* Node_ = nullptr;
    std::shared_ptr<const TRuntimeFilterSinkCode> Code_;
    std::shared_ptr<void> State_;
    bool Sent_ = false;
    std::atomic<bool> DependencyNotified_ = false;
};

// Hands the partial its code receives to a filter as one producer and
// finishes, which wakes the filter tasks behind its dependency edges.
class TRuntimeFilterSourceTask final : public ITask {
public:
    // Without a filter, a placeholder that never runs.
    explicit TRuntimeFilterSourceTask(std::shared_ptr<IRuntimeFilterProducer> filter = nullptr);

    ETaskResult Execute() override;

    // Only for a task built with empty code, before it is scheduled.
    void BindCode(std::shared_ptr<const TRuntimeFilterSourceCode> code, std::shared_ptr<void> state);
    const std::shared_ptr<const TRuntimeFilterSourceCode>& Code() const;

private:
    std::shared_ptr<IRuntimeFilterProducer> Filter_;
    std::shared_ptr<const TRuntimeFilterSourceCode> Code_;
    std::shared_ptr<void> State_;
    bool Finished_ = false;
};

class TUnaryTask final : public ITask {
public:
    TUnaryTask(
        std::shared_ptr<const TUnaryCode> code,
        std::shared_ptr<void> state,
        TInputPort input,
        TOutputPort output);
    ~TUnaryTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool RebindPort(const TPortRebind& rebind) override;

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

class TSinkTask final : public ITask {
public:
    TSinkTask(
        std::shared_ptr<const TSinkCode> code,
        std::shared_ptr<void> state,
        TInputPort input);
    ~TSinkTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool RebindPort(const TPortRebind& rebind) override;

    // Only for a task built with empty code (a remote sink), before it is scheduled.
    void BindCode(std::shared_ptr<const TSinkCode> code, std::shared_ptr<void> state);
    const std::shared_ptr<const TSinkCode>& Code() const;
    const IConnection* InputConnection() const;

private:
    std::shared_ptr<const TSinkCode> Code_;
    std::shared_ptr<void> State_;
    TInputPort Input_;
    TRowSet Pending_{};
    bool HasPending_ = false;
    bool InputFinished_ = false;
    bool Finished_ = false;
};

class TBlockingTask final : public ITask {
public:
    TBlockingTask(
        std::shared_ptr<const TBlockingCode> code,
        std::shared_ptr<ITaskState> state,
        TInputPort input,
        TOutputPort output);
    ~TBlockingTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool RebindPort(const TPortRebind& rebind) override;

private:
    std::shared_ptr<const TBlockingCode> Code_;
    std::shared_ptr<ITaskState> State_;
    TInputPort Input_;
    TOutputPort Output_;
    TRowSet CurrentOutput_ = {};
    bool HasOutput_ = false;
    bool OutputFinished_ = false;
};

class TBinaryBlockingTask final : public ITask {
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
    bool RebindPort(const TPortRebind& rebind) override;
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
class TMergeTask final : public ITask {
public:
    TMergeTask(
        std::shared_ptr<const TMergeCode> code,
        std::shared_ptr<ITaskState> state,
        std::vector<TInputPort> inputs,
        TOutputPort output);
    ~TMergeTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool RebindPort(const TPortRebind& rebind) override;

private:
    std::shared_ptr<const TMergeCode> Code_;
    std::shared_ptr<ITaskState> State_;
    std::vector<TInputPort> Inputs_;
    TOutputPort Output_;
    TRowSet CurrentOutput_ = {};
    bool HasOutput_ = false;
    bool OutputFinished_ = false;
};

// Routes each row by its key hash to one output per destination partition.
class THashShuffleTask final : public ITask {
public:
    THashShuffleTask(
        std::shared_ptr<const THashShuffleCode> code,
        TInputPort input,
        std::vector<TOutputPort> outputs);
    ~THashShuffleTask() override;

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool RebindPort(const TPortRebind& rebind) override;

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
    TInputPort Input_;
    std::vector<TOutputPort> Outputs_;
    std::unique_ptr<TInstrumentationState> ResourceInstrumentation_;
    std::optional<TRuntimeFilterBuilder> FilterBuilder_;
    std::vector<std::unique_ptr<TDestinationBuffer>> Buffers_;
    std::vector<TPendingOutput> Pending_;
    size_t PendingIndex_ = 0;
    bool InputFinished_ = false;
    bool OutputFinished_ = false;
};

} // namespace NScheduler
} // namespace NQdb
