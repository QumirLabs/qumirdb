#include <qdb/scheduler/tasks/cte_spool.h>

#include <qdb/exec/instrumentation/operator.h>
#include <qdb/scheduler/instrumentation/task.h>
#include <qdb/scheduler/tasks/runtime_adapter.h>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
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

TRowSet SpoolView(const std::shared_ptr<const TCteSpool>& spool, const TRowSet& batch) {
    TRowSet view = batch;
    view.Destroy = DestroyCteView;
    view.Private = new TCteViewData{spool};
    view.RefCount = 1;
    return view;
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

TCteSpool::TCteSpool(size_t segments, std::vector<TValueLayout> rowLayouts)
    : Segments_(segments)
    , RowLayouts_(std::move(rowLayouts))
{}

TCteSpool::~TCteSpool() {
    for (auto& segment : Segments_) {
        for (auto& batch : segment.Batches) {
            Release(&batch);
        }
    }
}

void TCteSpool::Append(size_t segment, TRowSet rowSet) {
    assert(SegmentEnds_.empty());
    {
        auto& target = Segments_[segment];
        std::lock_guard lock(target.Lock);
        target.Batches.push_back(rowSet);
    }
    // A reader armed before this append may have missed its batch: wake it.
    for (auto* watcher : Watchers_) {
        if (watcher->Armed.load() && watcher->Armed.exchange(false)) {
            watcher->Wake();
        }
    }
}

void TCteSpool::Seal() {
    assert(SegmentEnds_.empty());
    SegmentEnds_.reserve(Segments_.size());
    size_t end = 0;
    // Every writer has finished: the segments no longer change.
    for (const auto& segment : Segments_) {
        end += segment.Batches.size();
        SegmentEnds_.push_back(end);
    }
}

size_t TCteSpool::SegmentCount() const
{
    return Segments_.size();
}

std::span<const TValueLayout> TCteSpool::RowLayouts() const {
    return RowLayouts_;
}

void TCteSpool::Watch(TCteSpoolWatcher& watcher) {
    Watchers_.push_back(&watcher);
}

std::optional<TRowSet> TCteSpool::TryBatch(size_t segment, size_t index) const {
    const auto& source = Segments_[segment];
    std::lock_guard lock(source.Lock);
    if (index >= source.Batches.size()) {
        return std::nullopt;
    }
    return source.Batches[index];
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
    return Segments_[segment].Batches[index - begin];
}

TCteConsumerState::TCteConsumerState(std::shared_ptr<const TCteSpool> spool)
    : Spool_(std::move(spool))
{}

void TCteConsumerState::AddLane(size_t lane) {
    Lanes_.insert(std::ranges::upper_bound(Lanes_, lane), lane);
}

std::optional<size_t> TCteConsumerState::Claim() {
    // The spool is immutable after publication, so the cursor orders nothing.
    const size_t claim = Next_.fetch_add(1, std::memory_order_relaxed);
    // Claims walk the lanes' batches in ascending order: once one is past the
    // end, so are all later ones. With every lane present index == claim.
    const size_t index = claim / Lanes_.size() * Spool_->SegmentCount()
        + Lanes_[claim % Lanes_.size()];
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

bool TCteProducerTask::RebindPort(const TPortRebind& rebind) {
    return rebind.Apply(Input_) || rebind.Apply(Completion_);
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

bool TCtePublishTask::RebindPort(const TPortRebind& rebind) {
    return rebind.Apply(Producers_) || rebind.Apply(Consumers_);
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
    TRowSet view = SpoolView(spool, spool->Batch(*Claimed_));
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

bool TCteConsumerTask::RebindPort(const TPortRebind& rebind) {
    return rebind.Apply(Completion_) || rebind.Apply(Output_);
}

TCteSinkTask::TCteSinkTask(
    std::shared_ptr<TCteSpool> spool,
    std::vector<uint32_t> segments,
    std::vector<TInputPort> inputs)
    : Spool_(std::move(spool))
    , Segments_(std::move(segments))
    , Inputs_(std::move(inputs))
    , Next_(Segments_.size())
{
    if (Spool_) {
        RowLayouts_.assign(Spool_->RowLayouts().begin(), Spool_->RowLayouts().end());
    }
}

ETaskResult TCteSinkTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }
    // A running task reads every batch: only a wait needs a wake.
    Watcher_.Armed.store(false);
    for (bool armed = false;;) {
        // A writer finishes after its last append, so a pass after the finish sends everything.
        if (!InputFinished_) {
            InputFinished_ = std::ranges::all_of(Inputs_, [](auto& input) {
                return FetchCompletion(input) == EFetchResult::FINISHED;
            });
        }
        const bool inputFinished = InputFinished_;
        bool sent = false;
        if (!SendAppended(sent)) {
            return ETaskResult::WAITING;
        }
        if (inputFinished) {
            break;
        }
        if (sent) {
            armed = false;
        } else if (Watcher_.Wake && !armed) {
            // One more pass: an append it misses comes after the arm and wakes the task.
            Watcher_.Armed.store(true);
            armed = true;
        } else if (Watcher_.Wake && Pulled_) {
            // The next append or the finish of an input lane wakes the task.
            return ETaskResult::WAITING;
        } else {
            // Pulls the inputs, whose writers then run on their own input.
            Pulled_ = true;
            return ETaskResult::NEED_DATA;
        }
    }
    if (Code_->Finish && Code_->Finish(State_.get()) == ETaskResult::WAITING) {
        return ETaskResult::WAITING;
    }
    Spool_.reset();
    Finished_ = true;
    return ETaskResult::FINISHED;
}

bool TCteSinkTask::SendAppended(bool& sent) {
    for (size_t i = 0; i < Segments_.size(); ++i) {
        while (const auto batch = Spool_->TryBatch(Segments_[i], Next_[i])) {
            // The code takes its own reference if it keeps the view.
            TRowSet view = SpoolView(Spool_, *batch);
            const auto result = Code_->Write(State_.get(), view, Segments_[i]);
            Release(&view);
            if (result == ETaskResult::WAITING) {
                return false;
            }
            ++Next_[i];
            sent = true;
        }
    }
    return true;
}

bool TCteSinkTask::RebindPort(const TPortRebind& rebind) {
    return std::ranges::any_of(Inputs_, [&](auto& input) { return rebind.Apply(input); });
}

void TCteSinkTask::BindCode(
    std::shared_ptr<const TCteSinkCode> code, std::shared_ptr<void> state, std::function<void()> wake)
{
    assert(!Code_);
    Code_ = std::move(code);
    State_ = std::move(state);
    if (wake) {
        Watcher_.Wake = std::move(wake);
        Spool_->Watch(Watcher_);
    }
}

const std::shared_ptr<const TCteSinkCode>& TCteSinkTask::Code() const {
    return Code_;
}

std::span<const TValueLayout> TCteSinkTask::RowLayouts() const {
    return RowLayouts_;
}

TCteSourceTask::TCteSourceTask(
    std::shared_ptr<TCteSpool> spool,
    bool seal,
    std::vector<TOutputPort> outputs)
    : Spool_(std::move(spool))
    , Seal_(seal)
    , Outputs_(std::move(outputs))
{}

ETaskResult TCteSourceTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }
    for (;;) {
        TRowSet rowSet{};
        uint32_t segment = 0;
        switch (Code_->Next(State_.get(), rowSet, segment)) {
            case ETaskResult::OK:
                if (segment >= Spool_->SegmentCount()) {
                    Release(&rowSet);
                    throw std::runtime_error("cte source: a batch of segment " + std::to_string(segment)
                        + " for a spool of " + std::to_string(Spool_->SegmentCount()));
                }
                Spool_->Append(segment, rowSet);
                break;
            case ETaskResult::WAITING:
                return ETaskResult::WAITING;
            default:
                if (Seal_) {
                    Spool_->Seal();
                }
                Spool_.reset();
                Finished_ = true;
                for (const auto& output : Outputs_) {
                    output.Finish();
                }
                return ETaskResult::FINISHED;
        }
    }
}

bool TCteSourceTask::RebindPort(const TPortRebind& rebind) {
    return std::ranges::any_of(Outputs_, [&](auto& output) { return rebind.Apply(output); });
}

void TCteSourceTask::BindCode(std::shared_ptr<const TCteSourceCode> code, std::shared_ptr<void> state) {
    assert(!Code_);
    Code_ = std::move(code);
    State_ = std::move(state);
}

const std::shared_ptr<const TCteSourceCode>& TCteSourceTask::Code() const {
    return Code_;
}

} // namespace NScheduler
} // namespace NQdb
