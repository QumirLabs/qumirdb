#pragma once

#include <qdb/io/io.h>
#include <qdb/io/rowset_ops.h>
#include <qdb/scheduler/port.h>
#include <qdb/scheduler/task.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace NQdb {

struct TOperatorInstrumentation;

namespace NScheduler {

// The code a compute actor binds to a CTE sink: Write and Finish as in
// TSinkCode, with the spool segment of each batch.
struct TCteSinkCode {
    using TWrite = std::function<ETaskResult(void*, const TRowSet&, uint32_t segment)>;
    using TFinish = std::function<ETaskResult(void*)>;

    TWrite Write;
    TFinish Finish;
};

// The code a compute actor binds to a CTE source: Next as in TSourceCode, with
// the spool segment the batch was sent from.
struct TCteSourceCode {
    using TNext = std::function<ETaskResult(void*, TRowSet&, uint32_t& segment)>;

    TNext Next;
};

// A reader that streams a spool before Seal (a CTE sink). Once it sets Armed,
// the next Append clears it and calls Wake, so a reader that has found nothing
// new can wait.
struct TCteSpoolWatcher {
    std::function<void()> Wake;
    std::atomic<bool> Armed = false;
};

// The rows of a materialized CTE. Each segment has one writer, a producer lane
// or a CTE source. Before Seal, CTE sinks read a segment while it is appended
// to, under the segment's lock; once sealed the spool is immutable and read
// concurrently without locks. Writers, readers and their views share ownership.
class TCteSpool {
public:
    explicit TCteSpool(size_t segments, std::vector<TValueLayout> rowLayouts = {});
    ~TCteSpool();

    TCteSpool(const TCteSpool&) = delete;
    TCteSpool& operator=(const TCteSpool&) = delete;

    // Producer side, before Seal. The spool takes over the rowset reference
    // and wakes the armed watchers.
    void Append(size_t segment, TRowSet rowSet);
    void Seal();
    size_t SegmentCount() const;
    std::span<const TValueLayout> RowLayouts() const;

    // Streaming side, before the writers start; the watcher outlives their appends.
    void Watch(TCteSpoolWatcher& watcher);
    // Batch `index` of the segment once it is appended, also while the segment grows.
    std::optional<TRowSet> TryBatch(size_t segment, size_t index) const;

    // Consumer side, after Seal. Batches are numbered segment by segment.
    size_t BatchCount() const;
    const TRowSet& Batch(size_t index) const;

private:
    struct TSegment {
        mutable std::mutex Lock;
        std::vector<TRowSet> Batches;
    };

    std::vector<TSegment> Segments_;
    std::vector<TValueLayout> RowLayouts_;
    std::vector<TCteSpoolWatcher*> Watchers_;
    // Exclusive end of each segment in the batch numbering.
    std::vector<size_t> SegmentEnds_;
};

// The read progress of one CTE reference in one graph, shared by its lanes
// there. Batch i belongs to lane i mod SegmentCount(), so lanes of one
// reference in different graphs read disjoint batches. A lane claims the next
// unread batch of the graph's lanes, morsel-style, so each batch reaches
// exactly one lane and an idle lane takes the work a busy one has not started.
class TCteConsumerState {
public:
    explicit TCteConsumerState(std::shared_ptr<const TCteSpool> spool);

    // Before the first Claim: a lane of the reference in this graph.
    void AddLane(size_t lane);
    // Valid after publication; empty once every batch of the lanes has been claimed.
    std::optional<size_t> Claim();
    const std::shared_ptr<const TCteSpool>& Spool() const;

private:
    std::shared_ptr<const TCteSpool> Spool_;
    // Ascending.
    std::vector<size_t> Lanes_;
    std::atomic<size_t> Next_ = 0;
};

// One lane of a CTE producer: appends its input to its own spool segment and
// finishes its completion lane at the end of the input. The spool has no
// capacity limit, so no consumer can block the producer.
class TCteProducerTask final : public ITask {
public:
    TCteProducerTask(
        std::shared_ptr<TCteSpool> spool,
        size_t segment,
        TInputPort input,
        TOutputPort completion);

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool RebindPort(const TPortRebind& rebind) override;

private:
    std::shared_ptr<TCteSpool> Spool_;
    size_t Segment_ = 0;
    TInputPort Input_;
    TOutputPort Completion_;
    TOperatorInstrumentation* Instrumentation_ = nullptr;
    bool Finished_ = false;
};

// The publication barrier of a spool. When every producer lane has finished
// its completion lane, it seals the spool and finishes the completion lane of
// every consumer lane. Both connections carry finish signals only.
class TCtePublishTask final : public ITask {
public:
    TCtePublishTask(
        std::shared_ptr<TCteSpool> spool,
        TInputPort producers,
        TOutputPort consumers);

    ETaskResult Execute() override;
    bool RebindPort(const TPortRebind& rebind) override;

private:
    std::shared_ptr<TCteSpool> Spool_;
    TInputPort Producers_;
    TOutputPort Consumers_;
    bool Finished_ = false;
};

// One lane of a CTE reference. After publication it emits a view of each batch
// it claims; a view keeps the spool alive while downstream operators hold it.
class TCteConsumerTask final : public ITask {
public:
    TCteConsumerTask(
        std::shared_ptr<TCteConsumerState> state,
        TInputPort completion,
        TOutputPort output);

    ETaskResult Execute() override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;
    bool RebindPort(const TPortRebind& rebind) override;

private:
    std::shared_ptr<TCteConsumerState> State_;
    TInputPort Completion_;
    TOutputPort Output_;
    // A claimed batch whose push was refused; no other lane can take it.
    std::optional<size_t> Claimed_;
    bool Published_ = false;
    bool Finished_ = false;
};

// Sends the batches of Segments to another fragment, with their segment,
// through code the compute actor binds: the segments of this fragment's
// producer lanes, or every segment of the published spool. It streams each
// batch as it is appended and finishes once every input lane has finished.
class TCteSinkTask final : public ITask {
public:
    // Without a spool, a placeholder with unbound inputs.
    explicit TCteSinkTask(
        std::shared_ptr<TCteSpool> spool = nullptr,
        std::vector<uint32_t> segments = {},
        std::vector<TInputPort> inputs = {});

    ETaskResult Execute() override;
    bool RebindPort(const TPortRebind& rebind) override;

    // Only for a task built with empty code, before it is scheduled. `wake`
    // schedules the task after an append it waits for; without it the task
    // waits for its inputs to finish.
    void BindCode(std::shared_ptr<const TCteSinkCode> code, std::shared_ptr<void> state, std::function<void()> wake = {});
    const std::shared_ptr<const TCteSinkCode>& Code() const;
    // Of the rows it sends; valid while the task lives.
    std::span<const TValueLayout> RowLayouts() const;

private:
    // Passes every batch appended so far to Write; false on WAITING.
    bool SendAppended(bool& sent);

    std::shared_ptr<TCteSpool> Spool_;
    std::vector<uint32_t> Segments_;
    // A copy: the spool goes once everything is sent.
    std::vector<TValueLayout> RowLayouts_;
    std::vector<TInputPort> Inputs_;
    std::shared_ptr<const TCteSinkCode> Code_;
    std::shared_ptr<void> State_;
    TCteSpoolWatcher Watcher_;
    // The next batch to send of each of Segments_.
    std::vector<size_t> Next_;
    bool Pulled_ = false;
    bool InputFinished_ = false;
    bool Finished_ = false;
};

// Appends each batch its code receives to the segment it was sent from, so a
// copy numbers the batches as the spool it copies does. At their end it seals
// the spool if it is this fragment's copy of a published spool, then finishes
// every output lane.
class TCteSourceTask final : public ITask {
public:
    // Without a spool, a placeholder with unbound outputs.
    explicit TCteSourceTask(
        std::shared_ptr<TCteSpool> spool = nullptr,
        bool seal = false,
        std::vector<TOutputPort> outputs = {});

    ETaskResult Execute() override;
    bool RebindPort(const TPortRebind& rebind) override;

    // Only for a task built with empty code, before it is scheduled.
    void BindCode(std::shared_ptr<const TCteSourceCode> code, std::shared_ptr<void> state);
    const std::shared_ptr<const TCteSourceCode>& Code() const;

private:
    std::shared_ptr<TCteSpool> Spool_;
    bool Seal_ = false;
    std::vector<TOutputPort> Outputs_;
    std::shared_ptr<const TCteSourceCode> Code_;
    std::shared_ptr<void> State_;
    bool Finished_ = false;
};

} // namespace NScheduler
} // namespace NQdb
