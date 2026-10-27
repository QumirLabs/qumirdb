#include <qdb/scheduler/cte_spool.h>

#include <qdb/exec/instrumentation/operator.h>
#include <qdb/scheduler/instrumentation/task.h>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace NQdb {
namespace NScheduler {
namespace {

// A consumer's view of a spooled batch keeps the spool alive, so a rowset
// retained past the end of the plan (by a sink, for example) stays valid.
struct TCteViewData {
    std::shared_ptr<const TCteSpool> Spool;
};

void DestroyCteView(TRowSet* rowSet) {
    delete static_cast<TCteViewData*>(rowSet->Private);
}

// Completion connections carry finish signals only.
EFetchResult FetchCompletion(TInputPort& port) {
    TRowSet rowSet{};
    const auto result = port.Fetch(rowSet);
    if (result == EFetchResult::OK) {
        Release(&rowSet);
        throw std::runtime_error("cte spool: a completion connection carried a rowset");
    }
    return result;
}

} // namespace

TCteSpool::TCteSpool(size_t segments)
    : Segments_(segments)
{}

TCteSpool::~TCteSpool() {
    for (auto& segment : Segments_) {
        for (auto& batch : segment) {
            Release(&batch);
        }
    }
}

void TCteSpool::Append(size_t segment, TRowSet rowSet) {
    assert(SegmentEnds_.empty());
    Segments_[segment].push_back(rowSet);
}

void TCteSpool::Seal() {
    assert(SegmentEnds_.empty());
    SegmentEnds_.reserve(Segments_.size());
    size_t end = 0;
    for (const auto& segment : Segments_) {
        end += segment.size();
        SegmentEnds_.push_back(end);
    }
}

size_t TCteSpool::BatchCount() const {
    assert(SegmentEnds_.size() == Segments_.size());
    return SegmentEnds_.empty() ? 0 : SegmentEnds_.back();
}

const TRowSet& TCteSpool::Batch(size_t index) const {
    assert(index < BatchCount());
    const auto end = std::upper_bound(
        SegmentEnds_.begin(), SegmentEnds_.end(), index);
    const auto segment = static_cast<size_t>(end - SegmentEnds_.begin());
    const size_t begin = segment == 0
        ? 0
        : SegmentEnds_[segment - 1];
    return Segments_[segment][index - begin];
}

TCteConsumerState::TCteConsumerState(std::shared_ptr<const TCteSpool> spool)
    : Spool_(std::move(spool))
{}

std::optional<size_t> TCteConsumerState::Claim() {
    // The spool is immutable after publication, so the cursor orders nothing.
    const size_t index = Next_.fetch_add(1, std::memory_order_relaxed);
    if (index >= Spool_->BatchCount()) {
        return std::nullopt;
    }
    return index;
}

const std::shared_ptr<const TCteSpool>& TCteConsumerState::Spool() const {
    return Spool_;
}

TCteProducerTask::TCteProducerTask(
    std::shared_ptr<TCteSpool> spool,
    size_t segment,
    TInputPort input,
    TOutputPort completion)
    : Spool_(std::move(spool))
    , Segment_(segment)
    , Input_(input)
    , Completion_(completion)
{}

ETaskResult TCteProducerTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }
    for (;;) {
        TRowSet rowSet{};
        switch (Input_.Fetch(rowSet)) {
            case EFetchResult::OK:
                if (Instrumentation_) {
                    Instrumentation_->RetainInput(rowSet, 0);
                    Instrumentation_->Output.Add(rowSet);
                }
                Spool_->Append(Segment_, rowSet);
                break;
            case EFetchResult::NO_DATA:
                return ETaskResult::NEED_DATA;
            case EFetchResult::FINISHED:
                // The finish signal publishes this segment to the barrier.
                Spool_.reset();
                Finished_ = true;
                Completion_.Finish();
                return ETaskResult::FINISHED;
        }
    }
}

void TCteProducerTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Input(Input_);
    setup.Output(Completion_);
    Instrumentation_ = &setup.Counters();
}

TCtePublishTask::TCtePublishTask(
    std::shared_ptr<TCteSpool> spool,
    TInputPort producers,
    TOutputPort consumers)
    : Spool_(std::move(spool))
    , Producers_(producers)
    , Consumers_(consumers)
{}

ETaskResult TCtePublishTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }
    if (FetchCompletion(Producers_) == EFetchResult::NO_DATA) {
        return ETaskResult::NEED_DATA;
    }
    Spool_->Seal();
    Spool_.reset();
    Finished_ = true;
    Consumers_.Finish();
    return ETaskResult::FINISHED;
}

TCteConsumerTask::TCteConsumerTask(
    std::shared_ptr<TCteConsumerState> state,
    TInputPort completion,
    TOutputPort output)
    : State_(std::move(state))
    , Completion_(completion)
    , Output_(output)
{}

ETaskResult TCteConsumerTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }
    if (!Published_) {
        if (FetchCompletion(Completion_) == EFetchResult::NO_DATA) {
            return ETaskResult::NEED_DATA;
        }
        Published_ = true;
    }
    // Claim only what can be pushed now, leaving the rest to the other lanes.
    if (!Output_.CanPush()) {
        return ETaskResult::BLOCKED_OUTPUT;
    }
    if (!Claimed_) {
        Claimed_ = State_->Claim();
        if (!Claimed_) {
            // The reference's share of the spool goes with its last lane.
            State_.reset();
            Finished_ = true;
            Output_.Finish();
            return ETaskResult::FINISHED;
        }
    }

    const auto& spool = State_->Spool();
    TRowSet view = spool->Batch(*Claimed_);
    view.Destroy = DestroyCteView;
    view.Private = new TCteViewData{spool};
    view.RefCount = 1;
    if (!Output_.Push(std::move(view))) {
        Release(&view);
        return ETaskResult::BLOCKED_OUTPUT;
    }
    Claimed_.reset();
    return ETaskResult::OK;
}

void TCteConsumerTask::ConfigureInstrumentation(TTaskInstrumentationSetup& setup) {
    setup.Output(Output_);
}

} // namespace NScheduler
} // namespace NQdb
