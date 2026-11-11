#include <qdb/scheduler/connections/local.h>

#include <qdb/scheduler/utils/spsc.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <utility>

namespace NQdb {
namespace NScheduler {
namespace {

TRowSet TakeRowSet(TRowSet& rowSet) {
    auto out = rowSet;
    rowSet = {};
    return out;
}

// Holds the shared source rowset behind a broadcast copy; releasing the last
// copy releases the original.
struct TBroadcastRowSetData {
    std::shared_ptr<TRowSet> Input;
};

void DestroyBroadcastRowSet(TRowSet* rowSet) {
    delete static_cast<TBroadcastRowSetData*>(rowSet->Private);
}

} // namespace

struct TConnectionLane {
    explicit TConnectionLane(size_t capacity)
        : Queue(capacity)
    {}

    ~TConnectionLane() {
        TRowSet rowSet{};
        while (Queue.TryPop(rowSet)) {
            Release(&rowSet);
        }
    }

    TSPSC<TRowSet> Queue;
    std::atomic<bool> Finished = false;
    std::atomic<bool> Closed = false;
    // A push found the queue full; the consumer takes the mark when it drained it.
    std::atomic<bool> PushBlocked = false;
};

namespace {

// Marks a full lane, then looks again: either this producer sees the room a pop
// made, or that consumer's later ParkedDemand sees the mark (both fence).
bool MarkFullLane(TConnectionLane& lane) {
    lane.PushBlocked.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return lane.Queue.CanPush();
}

} // namespace

bool TLocalConnection::LaneCanPush(TConnectionLane& lane) const {
    if (lane.Queue.CanPush() || MarkFullLane(lane)) {
        return true;
    }
    CountBlockedPush();
    return false;
}

bool TLocalConnection::LanePush(TConnectionLane& lane, TRowSet& rowSet) const {
    auto moved = TakeRowSet(rowSet);
    if (lane.Queue.TryPush(std::move(moved))
        || (MarkFullLane(lane) && lane.Queue.TryPush(std::move(moved))))
    {
        CountPushed();
        return true;
    }
    CountBlockedPush();
    rowSet = TakeRowSet(moved);
    return false;
}

bool TLocalConnection::LaneHasBlockedPush(const TConnectionLane& lane) {
    return lane.PushBlocked.load(std::memory_order_relaxed);
}

EParkedDemand TLocalConnection::LaneParkedDemand(TConnectionLane& lane) {
    // Pairs with the fence in MarkFullLane: the pops of this consumer come first.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (lane.PushBlocked.load(std::memory_order_relaxed)
        && lane.Queue.CanPush()
        && lane.PushBlocked.exchange(false, std::memory_order_relaxed))
    {
        return EParkedDemand::WakeProducer;
    }
    // A task may ask for more after one batch (a join takes one per run): rows left
    // in the lane need another run of it, not of the parked producer.
    return lane.Queue.Empty() ? EParkedDemand::Wait : EParkedDemand::RerunConsumer;
}

TOneToOneConnection::TOneToOneConnection(size_t capacity)
    : Capacity_(capacity)
{
    Resize(1, 1);
}

TOneToOneConnection::~TOneToOneConnection() = default;

EConnectionKind TOneToOneConnection::Kind() const {
    return EConnectionKind::OneToOne;
}

size_t TOneToOneConnection::Capacity() const {
    return Capacity_;
}

void TOneToOneConnection::Resize(size_t srcCount, size_t dstCount) {
    assert(srcCount == dstCount);
    Size_ = srcCount;
    Lanes_.clear();
    Lanes_.reserve(Size_);
    for (size_t i = 0; i < Size_; ++i) {
        Lanes_.push_back(std::make_unique<TConnectionLane>(Capacity_));
    }
}

size_t TOneToOneConnection::SrcCount() const {
    return Size_;
}

size_t TOneToOneConnection::DstCount() const {
    return Size_;
}

bool TOneToOneConnection::CanPush(size_t srcId) const {
    assert(srcId < Size_);
    return LaneCanPush(*Lanes_[srcId]);
}

bool TOneToOneConnection::Push(size_t srcId, TRowSet&& rowSet) {
    assert(srcId < Size_);
    return LanePush(*Lanes_[srcId], rowSet);
}

void TOneToOneConnection::Finish(size_t srcId) {
    assert(srcId < Size_);
    if (!Lanes_[srcId]->Finished.exchange(true, std::memory_order_acq_rel)) {
        CountFinished();
    }
}

EFetchResult TOneToOneConnection::Fetch(size_t dstId, TRowSet& rowSet) {
    assert(dstId < Size_);
    if (Lanes_[dstId]->Queue.TryPop(rowSet)) {
        CountPopped();
        return EFetchResult::OK;
    }

    if (!Lanes_[dstId]->Finished.load(std::memory_order_acquire)) {
        CountEmptyFetch();
        return EFetchResult::NO_DATA;
    }
    if (Lanes_[dstId]->Queue.TryPop(rowSet)) {
        CountPopped();
        return EFetchResult::OK;
    }
    CountFinishedFetch();
    return EFetchResult::FINISHED;
}

bool TOneToOneConnection::HasBlockedPush(size_t srcId) const {
    assert(srcId < Size_);
    return LaneHasBlockedPush(*Lanes_[srcId]);
}

EParkedDemand TOneToOneConnection::ParkedDemand(size_t srcId, size_t) {
    assert(srcId < Size_);
    return LaneParkedDemand(*Lanes_[srcId]);
}

TGatherConnection::TGatherConnection(size_t capacity)
    : Capacity_(capacity)
{
    Resize(1, 1);
}

TGatherConnection::~TGatherConnection() = default;

EConnectionKind TGatherConnection::Kind() const {
    return EConnectionKind::Gather;
}

size_t TGatherConnection::Capacity() const {
    return Capacity_;
}

void TGatherConnection::Resize(size_t srcCount, size_t dstCount) {
    assert(dstCount == 1);
    Size_ = srcCount;
    FetchId_ = 0;
    FinishedCount_.store(0, std::memory_order_release);
    Lanes_.clear();
    Lanes_.reserve(Size_);
    for (size_t i = 0; i < Size_; ++i) {
        Lanes_.push_back(std::make_unique<TConnectionLane>(Capacity_));
    }
}

size_t TGatherConnection::SrcCount() const {
    return Size_;
}

size_t TGatherConnection::DstCount() const {
    return 1;
}

bool TGatherConnection::CanPush(size_t srcId) const {
    assert(srcId < Size_);
    return LaneCanPush(*Lanes_[srcId]);
}

bool TGatherConnection::Push(size_t srcId, TRowSet&& rowSet) {
    assert(srcId < Size_);
    return LanePush(*Lanes_[srcId], rowSet);
}

void TGatherConnection::Finish(size_t srcId) {
    assert(srcId < Size_);
    if (!Lanes_[srcId]->Finished.exchange(true, std::memory_order_acq_rel)) {
        FinishedCount_.fetch_add(1, std::memory_order_acq_rel);
        CountFinished();
    }
}

EFetchResult TGatherConnection::Fetch(size_t dstId, TRowSet& rowSet) {
    assert(dstId == 0);
    if (Size_ == 0) {
        return EFetchResult::FINISHED;
    }

    auto fetch = [&]() {
        auto from = (FetchId_ + 1) % Size_;
        FetchId_ = from;
        for (size_t i = 0; i < Size_; ++i) {
            auto index = (from + i) % Size_;
            if (Lanes_[index]->Queue.TryPop(rowSet)) {
                CountPopped();
                return true;
            }
        }
        return false;
    };

    if (fetch()) {
        return EFetchResult::OK;
    }

    if (FinishedCount_.load(std::memory_order_acquire) != Size_) {
        CountEmptyFetch();
        return EFetchResult::NO_DATA;
    }
    if (fetch()) {
        return EFetchResult::OK;
    }
    CountFinishedFetch();
    return EFetchResult::FINISHED;
}

bool TGatherConnection::HasBlockedPush(size_t srcId) const {
    assert(srcId < Size_);
    return LaneHasBlockedPush(*Lanes_[srcId]);
}

EParkedDemand TGatherConnection::ParkedDemand(size_t srcId, size_t) {
    assert(srcId < Size_);
    return LaneParkedDemand(*Lanes_[srcId]);
}

TBroadcastConnection::TBroadcastConnection(size_t capacity)
    : Capacity_(capacity)
{
    Resize(1, 1);
}

TBroadcastConnection::~TBroadcastConnection() = default;

EConnectionKind TBroadcastConnection::Kind() const {
    return EConnectionKind::Broadcast;
}

size_t TBroadcastConnection::Capacity() const {
    return Capacity_;
}

void TBroadcastConnection::Resize(size_t srcCount, size_t dstCount) {
    assert(srcCount == 1);
    DstCount_ = dstCount;
    Finished_.store(false, std::memory_order_release);
    Lanes_.clear();
    Lanes_.reserve(DstCount_);
    for (size_t i = 0; i < DstCount_; ++i) {
        Lanes_.push_back(std::make_unique<TConnectionLane>(Capacity_));
    }
}

size_t TBroadcastConnection::SrcCount() const {
    return 1;
}

size_t TBroadcastConnection::DstCount() const {
    return DstCount_;
}

bool TBroadcastConnection::CanPush(size_t srcId) const {
    assert(srcId == 0);
    // With every lane closed, block like a full lane: the producer parks instead
    // of producing rows nobody reads.
    bool open = false;
    bool blocked = false;
    for (const auto& lane : Lanes_) {
        if (lane->Closed.load(std::memory_order_acquire)) {
            continue;
        }
        open = true;
        // Every full lane is marked, so draining any of them wakes the producer.
        if (!lane->Queue.CanPush() && !MarkFullLane(*lane)) {
            blocked = true;
        }
    }
    if (!open || blocked) {
        CountBlockedPush();
        return false;
    }
    return true;
}

bool TBroadcastConnection::Push(size_t srcId, TRowSet&& rowSet) {
    assert(srcId == 0);
    // All open destination lanes must accept, so the rowset is replicated to each
    // in lockstep; otherwise leave the rowset untouched for a retry.
    if (!CanPush(srcId)) {
        return false;
    }

    auto moved = TakeRowSet(rowSet);
    auto input = std::shared_ptr<TRowSet>(
        new TRowSet(moved),
        [](TRowSet* p) {
            Release(p);
            delete p;
        });
    for (auto& lane : Lanes_) {
        // Closed lanes get no copy; with none left, `input` releases the rowset.
        if (lane->Closed.load(std::memory_order_acquire)) {
            continue;
        }
        auto* data = new TBroadcastRowSetData{input};
        TRowSet copy{
            .Columns = input->Columns,
            .ColumnCount = input->ColumnCount,
            .RowCount = input->RowCount,
            .Selection = input->Selection,
            .Destroy = DestroyBroadcastRowSet,
            .Private = data,
            .RefCount = 1,
        };
        const bool pushed = lane->Queue.TryPush(std::move(copy));
        assert(pushed);
        (void)pushed;
    }
    CountPushed();
    return true;
}

void TBroadcastConnection::Finish(size_t srcId) {
    assert(srcId == 0);
    if (!Finished_.exchange(true, std::memory_order_acq_rel)) {
        CountFinished();
    }
}

EFetchResult TBroadcastConnection::Fetch(size_t dstId, TRowSet& rowSet) {
    assert(dstId < Lanes_.size());
    if (Lanes_[dstId]->Queue.TryPop(rowSet)) {
        CountPopped();
        return EFetchResult::OK;
    }
    if (!Finished_.load(std::memory_order_acquire)) {
        CountEmptyFetch();
        return EFetchResult::NO_DATA;
    }
    if (Lanes_[dstId]->Queue.TryPop(rowSet)) {
        CountPopped();
        return EFetchResult::OK;
    }
    CountFinishedFetch();
    return EFetchResult::FINISHED;
}

void TBroadcastConnection::Close(size_t dstId) {
    assert(dstId < Lanes_.size());
    Lanes_[dstId]->Closed.store(true, std::memory_order_release);
}

bool TBroadcastConnection::HasBlockedPush(size_t) const {
    for (const auto& lane : Lanes_) {
        if (LaneHasBlockedPush(*lane)) {
            return true;
        }
    }
    return false;
}

EParkedDemand TBroadcastConnection::ParkedDemand(size_t, size_t dstId) {
    assert(dstId < Lanes_.size());
    return LaneParkedDemand(*Lanes_[dstId]);
}

} // namespace NScheduler
} // namespace NQdb
