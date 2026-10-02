#pragma once

#include <qdb/exec/runtime_filter.h>
#include <qdb/io/io.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/state.h>

#include <qumir/parser/type.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace NQdb {
namespace NScheduler {

enum class ERequiredSide { Both = 0, Left = 1, Right = 2 };

struct TInputPort {
    IConnection* Connection = nullptr;
    size_t Lane = 0;

    EFetchResult Fetch(TRowSet& rowSet) const;
};

struct TOutputPort {
    IConnection* Connection = nullptr;
    size_t Lane = 0;

    bool CanPush() const;
    bool Push(TRowSet&& rowSet) const;
    void Finish() const;
};

struct TSourceCode {
    using TNext = std::function<bool(void*, TRowSet&)>;

    explicit TSourceCode(TNext next);

    TNext Next;
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
    using TProcess = std::function<ETaskResult(void*, TInputPort&, TRowSet&)>;

    explicit TBlockingCode(TProcess process);

    TProcess Process;
};

struct TBinaryBlockingCode {
    using TProcess = std::function<ETaskResult(
        void*,
        TInputPort&,
        TInputPort&,
        TRowSet&)>;
    // Which side Process will read next; empty => both. Consulted by the task's
    // WantsInput to gate readiness.
    using TRequiredSide = std::function<ERequiredSide(void*)>;

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
    size_t TargetOutputBatchBytes = 0;
    // Bindings are shared by every lane of one exchange. The local factory
    // uses one filter object; remote publishers and probes may be separate.
    // At most one of the two is set.
    std::shared_ptr<IRuntimeFilterProducer> ProducedFilter;
    std::shared_ptr<IRuntimeFilterProbe> AppliedFilter;
};

struct TMergeCode {
    using TProcess = std::function<ETaskResult(
        void*,
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

private:
    std::shared_ptr<const TSourceCode> Code_;
    std::shared_ptr<void> State_;
    TOutputPort Output_;
    bool Finished_ = false;
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
        std::shared_ptr<void> state,
        TInputPort input,
        TOutputPort output);
    ~TBlockingTask() override;

    ETaskResult Execute() override;

private:
    std::shared_ptr<const TBlockingCode> Code_;
    std::shared_ptr<void> State_;
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
        std::shared_ptr<void> state,
        TInputPort left,
        TInputPort right,
        TOutputPort output);
    ~TBinaryBlockingTask() override;

    ETaskResult Execute() override;
    bool WantsInput(const TTaskEdge& edge) const override;

private:
    std::shared_ptr<const TBinaryBlockingCode> Code_;
    std::shared_ptr<void> State_;
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
        std::shared_ptr<void> state,
        std::vector<TInputPort> inputs,
        TOutputPort output);
    ~TMergeTask() override;

    ETaskResult Execute() override;

private:
    std::shared_ptr<const TMergeCode> Code_;
    std::shared_ptr<void> State_;
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

private:
    struct TBufferedBatch;
    struct TDestinationBuffer;
    struct TPendingOutput;

    bool BatchingEnabled() const;
    bool NeedsHashes(size_t partitions) const;
    void AccumulateFilter(const TRowSet& rowSet);
    bool FilterKeeps(size_t row) const;
    void EnsureBuffers(size_t partitions);
    void Scatter(TRowSet& rowSet);
    void ScatterBuffered(TRowSet& rowSet);
    void ScatterViews(TRowSet& rowSet);
    void AddBufferedRows(size_t dst,
        const std::shared_ptr<TRowSet>& input,
        std::vector<int32_t>&& rows,
        size_t bytes,
        const std::shared_ptr<std::vector<uint64_t>>& hashes);
    void FlushBuffer(size_t dst);
    void FlushAllBuffers();
    ETaskResult PushPending();
    void ClearPending();

private:
    std::shared_ptr<const THashShuffleCode> Code_;
    std::shared_ptr<void> State_;
    TInputPort Input_;
    THashShuffleConnection* Output_ = nullptr;
    size_t SourceLane_ = 0;
    std::vector<uint64_t> Hashes_;
    std::optional<TRuntimeFilterBuilder> FilterBuilder_;
    std::vector<std::unique_ptr<TDestinationBuffer>> Buffers_;
    std::vector<TPendingOutput> Pending_;
    size_t PendingIndex_ = 0;
    bool OutputFinished_ = false;
};

// Consumers push non-owning views of Batches, so the spool must outlive every
// consumer's downstream (the graph owns both, so it does).
struct TCteSharedState {
    enum class EStatus {
        Building,
        Ready,
    };

    std::atomic<EStatus> Status = EStatus::Building;
    std::vector<TRowSet> Batches;

    ~TCteSharedState() {
        for (auto& batch : Batches) {
            Release(&batch);
        }
    }
};

class TCteConsumerTask final : public ITaskNode {
public:
    TCteConsumerTask(std::shared_ptr<const TCteSharedState> state, TOutputPort output);

    ETaskResult Execute() override;

private:
    std::shared_ptr<const TCteSharedState> State_;
    TOutputPort Output_;
    size_t NextBatch_ = 0;
    bool Finished_ = false;
};

} // namespace NScheduler
} // namespace NQdb
