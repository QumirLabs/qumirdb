#include <qdb/exec/runtime_filter.h>

#include <algorithm>
#include <bit>

namespace NQdb {

namespace {

// Reserve zero for empty slots.
uint64_t Storable(uint64_t hash) {
    return hash == 0 ? 1 : hash;
}

} // namespace

void TRuntimeFilterBuilder::Compact() {
    std::ranges::sort(Hashes_);
    const auto duplicates = std::ranges::unique(Hashes_);
    Hashes_.erase(duplicates.begin(), duplicates.end());
    if (Hashes_.size() > MaxKeys_) {
        Overflowed_ = true;
        Hashes_.clear();
        Hashes_.shrink_to_fit();
    }
}

TRuntimeFilterPartial TRuntimeFilterBuilder::TakePartial() && {
    // A nonempty contribution without key bounds cannot safely participate in
    // a global range, even if the caller did not explicitly call DropBounds().
    const bool boundsUsable = BoundsUsable_
        && (Bounds_.has_value() || (Hashes_.empty() && !Overflowed_));
    return TRuntimeFilterPartial{
        .Hashes = std::move(Hashes_),
        .Bounds = Bounds_
            ? std::optional(std::pair{Bounds_->Min, Bounds_->Max})
            : std::nullopt,
        .BoundsUsable = boundsUsable,
        .ExactSetOverflowed = Overflowed_,
    };
}

void TRuntimeFilter::Merge(TRuntimeFilterBuilder&& builder) {
    Merge(std::move(builder).TakePartial());
}

void TRuntimeFilter::Merge(TRuntimeFilterPartial&& partial) {
    std::lock_guard guard(Mutex_);
    if (!partial.BoundsUsable) {
        BoundsUsable_ = false;
    } else if (partial.Bounds) {
        if (!Bounds_) {
            Bounds_ = *partial.Bounds;
        } else {
            Bounds_->first = std::min(Bounds_->first, partial.Bounds->first);
            Bounds_->second = std::max(Bounds_->second, partial.Bounds->second);
        }
    }
    if (ExactSetDisabled() || partial.ExactSetOverflowed) {
        ExactSetDisabled_.store(true, std::memory_order_relaxed);
        Pending_.clear();
        Pending_.shrink_to_fit();
        return;
    }
    Pending_.insert(Pending_.end(),
        partial.Hashes.begin(), partial.Hashes.end());
    if (Pending_.size() >= MaxKeys_ * 2) {
        std::ranges::sort(Pending_);
        const auto duplicates = std::ranges::unique(Pending_);
        Pending_.erase(duplicates.begin(), duplicates.end());
        if (Pending_.size() > MaxKeys_) {
            ExactSetDisabled_.store(true, std::memory_order_relaxed);
            Pending_.clear();
            Pending_.shrink_to_fit();
        }
    }
}

void TRuntimeFilter::SetProducerCount(size_t producers) {
    PendingProducers_.store(producers, std::memory_order_relaxed);
}

void TRuntimeFilter::FinishProducer(TRuntimeFilterBuilder&& builder) {
    FinishProducer(std::move(builder).TakePartial());
}

void TRuntimeFilter::FinishProducer(TRuntimeFilterPartial&& partial) {
    Merge(std::move(partial));
    if (PendingProducers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        Publish();
    }
}

void TRuntimeFilter::Publish() {
    {
        std::lock_guard guard(Mutex_);
        if (!ExactSetDisabled()) {
            std::ranges::sort(Pending_);
            const auto duplicates = std::ranges::unique(Pending_);
            Pending_.erase(duplicates.begin(), duplicates.end());
            // Deferred compaction may leave the final distinct count over cap.
            if (Pending_.size() > MaxKeys_) {
                ExactSetDisabled_.store(true, std::memory_order_relaxed);
            } else {
                KeyCount_ = Pending_.size();
                // Half load keeps linear probes short.
                const size_t capacity =
                    std::bit_ceil(std::max<size_t>(8, KeyCount_ * 2));
                Table_.assign(capacity, 0);
                Mask_ = capacity - 1;
                for (uint64_t hash : Pending_) {
                    const uint64_t stored = Storable(hash);
                    uint64_t slot = hash & Mask_;
                    while (Table_[slot] != 0 && Table_[slot] != stored) {
                        slot = (slot + 1) & Mask_;
                    }
                    Table_[slot] = stored;
                }
            }
            Pending_.clear();
            Pending_.shrink_to_fit();
        }
    }
    Ready_.store(true, std::memory_order_release);
}

std::optional<std::pair<int64_t, int64_t>> TRuntimeFilter::Bounds() const {
    if (!Ready() || !BoundsUsable_ || !Bounds_) {
        return std::nullopt;
    }
    return Bounds_;
}

bool TRuntimeFilter::MayContain(uint64_t hash) const {
    if (!Ready() || ExactSetDisabled()) {
        return true;
    }
    const uint64_t stored = Storable(hash);
    uint64_t slot = hash & Mask_;
    while (true) {
        const uint64_t entry = Table_[slot];
        if (entry == 0) {
            return false;
        }
        if (entry == stored) {
            return true;
        }
        slot = (slot + 1) & Mask_;
    }
}

bool TRuntimeFilter::MayContainKey(int64_t value) const {
    if (!Ready() || !BoundsUsable_ || !Bounds_) {
        return true;
    }
    return value >= Bounds_->first && value <= Bounds_->second;
}

TRuntimeFilterBinding TLocalRuntimeFilterBindingFactory::Create(
    uint32_t id, size_t producerCount)
{
    (void)id;
    auto filter = std::make_shared<TRuntimeFilter>();
    filter->SetProducerCount(producerCount);
    return TRuntimeFilterBinding{
        .Producer = filter,
        .Probe = std::move(filter),
    };
}

} // namespace NQdb
