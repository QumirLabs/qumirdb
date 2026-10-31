#pragma once

#include <qdb/io/io.h>
#include <qdb/scheduler/runtime_adapter.h>
#include <qdb/scheduler/state.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace NQdb {
namespace NScheduler {

// The rows of a materialized CTE. Each producer lane owns one segment and
// appends to it without synchronization. Publication seals the spool once every
// producer lane has finished; from then on it is immutable and consumer lanes
// read it concurrently. Producer lanes, consumer references and the views they
// emit share ownership, so the rows are released when the last of them is done.
class TCteSpool {
public:
    explicit TCteSpool(size_t segments);
    ~TCteSpool();

    TCteSpool(const TCteSpool&) = delete;
    TCteSpool& operator=(const TCteSpool&) = delete;

    // Producer side, before Seal. The spool takes over the rowset reference.
    void Append(size_t segment, TRowSet rowSet);
    void Seal();

    // Consumer side, after Seal. Batches are numbered segment by segment.
    size_t BatchCount() const;
    const TRowSet& Batch(size_t index) const;

private:
    std::vector<std::vector<TRowSet>> Segments_;
    // Exclusive end of each segment in the batch numbering.
    std::vector<size_t> SegmentEnds_;
};

// The read progress of one CTE reference, shared by its lanes. A lane claims
// the next unread batch, morsel-style, so each batch reaches exactly one lane
// of the reference and an idle lane takes the work a busy one has not started.
class TCteConsumerState {
public:
    explicit TCteConsumerState(std::shared_ptr<const TCteSpool> spool);

    // Valid after publication; empty once every batch has been claimed.
    std::optional<size_t> Claim();
    const std::shared_ptr<const TCteSpool>& Spool() const;

private:
    std::shared_ptr<const TCteSpool> Spool_;
    std::atomic<size_t> Next_ = 0;
};

// One lane of a CTE producer: appends its input to its own spool segment and
// finishes its completion lane at the end of the input. The spool has no
// capacity limit, so no consumer can block the producer.
class TCteProducerTask final : public ITaskNode {
public:
    TCteProducerTask(
        std::shared_ptr<TCteSpool> spool,
        size_t segment,
        TInputPort input,
        TOutputPort completion);

    ETaskResult Execute(TExecutionContext& context) override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;

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
class TCtePublishTask final : public ITaskNode {
public:
    TCtePublishTask(
        std::shared_ptr<TCteSpool> spool,
        TInputPort producers,
        TOutputPort consumers);

    ETaskResult Execute(TExecutionContext& context) override;

private:
    std::shared_ptr<TCteSpool> Spool_;
    TInputPort Producers_;
    TOutputPort Consumers_;
    bool Finished_ = false;
};

// One lane of a CTE reference. After publication it emits a view of each batch
// it claims; a view keeps the spool alive while downstream operators hold it.
class TCteConsumerTask final : public ITaskNode {
public:
    TCteConsumerTask(
        std::shared_ptr<TCteConsumerState> state,
        TInputPort completion,
        TOutputPort output);

    ETaskResult Execute(TExecutionContext& context) override;
    void ConfigureInstrumentation(TTaskInstrumentationSetup& setup) override;

private:
    std::shared_ptr<TCteConsumerState> State_;
    TInputPort Completion_;
    TOutputPort Output_;
    // A claimed batch whose push was refused; no other lane can take it.
    std::optional<size_t> Claimed_;
    bool Published_ = false;
    bool Finished_ = false;
};

} // namespace NScheduler
} // namespace NQdb
