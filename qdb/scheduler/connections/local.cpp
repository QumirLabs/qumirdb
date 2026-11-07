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
};

bool TLocalConnection::LaneCanPush(TConnectionLane& lane) const {
    const bool canPush = lane.Queue.CanPush();
    if (!canPush) {
        CountBlockedPush();
    }
    return canPush;
}

bool TLocalConnection::LanePush(TConnectionLane& lane, TRowSet& rowSet) const {
    auto moved = TakeRowSet(rowSet);
    if (lane.Queue.TryPush(std::move(moved))) {
        CountPushed();
        return true;
    }
    CountBlockedPush();
    rowSet = TakeRowSet(moved);
    return false;
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

TGatherConnection::TGatherConnection(size_t capacity)
    : Capacity_(capacity)
{
    Resize(1, 1);
}

TGatherConnection::~TGatherConnection() = default;

EConnectionKind TGatherConnection::Kind() const {
    return EConnectionKind::Gather;
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

THashShuffleConnection::THashShuffleConnection(
    size_t capacity,
    TRepartitionSpec repartition)
    : Capacity_(capacity)
    , Repartition_(std::move(repartition))
{
    Resize(1, 1);
}

THashShuffleConnection::~THashShuffleConnection() = default;

EConnectionKind THashShuffleConnection::Kind() const {
    return EConnectionKind::HashShuffle;
}

void THashShuffleConnection::Resize(size_t srcCount, size_t dstCount) {
    SrcCount_ = srcCount;
    DstCount_ = dstCount;
    FinishedCount_.store(0, std::memory_order_release);
    Lanes_.clear();
    Lanes_.reserve(SrcCount_ * DstCount_);
    for (size_t i = 0; i < SrcCount_ * DstCount_; ++i) {
        Lanes_.push_back(std::make_unique<TConnectionLane>(Capacity_));
    }
    Finished_.clear();
    Finished_.reserve(SrcCount_);
    for (size_t i = 0; i < SrcCount_; ++i) {
        Finished_.push_back(std::make_unique<std::atomic<bool>>(false));
    }
    FetchIds_.assign(DstCount_, 0);
}

size_t THashShuffleConnection::SrcCount() const {
    return SrcCount_;
}

size_t THashShuffleConnection::DstCount() const {
    return DstCount_;
}

bool THashShuffleConnection::CanPush(size_t srcId) const {
    assert(DstCount_ == 1);
    return CanPushTo(srcId, 0);
}

bool THashShuffleConnection::Push(size_t srcId, TRowSet&& rowSet) {
    assert(DstCount_ == 1);
    return PushTo(srcId, 0, std::move(rowSet));
}

void THashShuffleConnection::Finish(size_t srcId) {
    assert(srcId < SrcCount_);
    if (!Finished_[srcId]->exchange(true, std::memory_order_acq_rel)) {
        FinishedCount_.fetch_add(1, std::memory_order_acq_rel);
        CountFinished();
    }
}

bool THashShuffleConnection::CanPushTo(size_t srcId, size_t dstId) const {
    assert(srcId < SrcCount_);
    assert(dstId < DstCount_);
    const bool canPush = Lanes_[LaneIndex(srcId, dstId)]->Queue.CanPush();
    if (!canPush) {
        CountBlockedPush();
    }
    return canPush;
}

bool THashShuffleConnection::PushTo(size_t srcId, size_t dstId, TRowSet&& rowSet) {
    assert(srcId < SrcCount_);
    assert(dstId < DstCount_);
    auto moved = TakeRowSet(rowSet);
    if (Lanes_[LaneIndex(srcId, dstId)]->Queue.TryPush(std::move(moved))) {
        CountPushed();
        return true;
    }
    CountBlockedPush();
    rowSet = TakeRowSet(moved);
    return false;
}

EFetchResult THashShuffleConnection::Fetch(size_t dstId, TRowSet& rowSet) {
    assert(dstId < DstCount_);
    if (SrcCount_ == 0) {
        return EFetchResult::FINISHED;
    }

    auto fetch = [&]() {
        auto from = (FetchIds_[dstId] + 1) % SrcCount_;
        FetchIds_[dstId] = from;
        for (size_t i = 0; i < SrcCount_; ++i) {
            auto srcId = (from + i) % SrcCount_;
            if (Lanes_[LaneIndex(srcId, dstId)]->Queue.TryPop(rowSet)) {
                CountPopped();
                return true;
            }
        }
        return false;
    };

    if (fetch()) {
        return EFetchResult::OK;
    }

    if (FinishedCount_.load(std::memory_order_acquire) != SrcCount_) {
        CountEmptyFetch();
        return EFetchResult::NO_DATA;
    }
    if (fetch()) {
        return EFetchResult::OK;
    }
    CountFinishedFetch();
    return EFetchResult::FINISHED;
}

const TRepartitionSpec& THashShuffleConnection::Repartition() const {
    return Repartition_;
}

size_t THashShuffleConnection::LaneIndex(size_t srcId, size_t dstId) const {
    return srcId * DstCount_ + dstId;
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
    for (const auto& lane : Lanes_) {
        if (!lane->Queue.CanPush()) {
            CountBlockedPush();
            return false;
        }
    }
    return true;
}

bool TBroadcastConnection::Push(size_t srcId, TRowSet&& rowSet) {
    assert(srcId == 0);
    // All destination lanes must accept, so the rowset is replicated to each in
    // lockstep; otherwise leave the rowset untouched for a retry.
    for (const auto& lane : Lanes_) {
        if (!lane->Queue.CanPush()) {
            CountBlockedPush();
            return false;
        }
    }

    auto moved = TakeRowSet(rowSet);
    auto input = std::shared_ptr<TRowSet>(
        new TRowSet(moved),
        [](TRowSet* p) {
            Release(p);
            delete p;
        });
    for (auto& lane : Lanes_) {
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

} // namespace NScheduler
} // namespace NQdb
