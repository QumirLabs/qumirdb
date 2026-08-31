#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace NQdb {

// Runtime filters may return false positives, never false negatives.
// Builders are task-local; probes pass until their merged state is published.
class TRuntimeFilterBuilder {
public:
    explicit TRuntimeFilterBuilder(size_t maxKeys) : MaxKeys_(maxKeys) {}

    void Add(uint64_t hash) {
        if (Overflowed_) {
            return;
        }
        Hashes_.push_back(hash);
        // Delay compaction to avoid sorting every duplicate at the cap.
        if (Hashes_.size() >= MaxKeys_ * 2) {
            Compact();
        }
    }

    // Bounds apply only to single-column integer keys.
    void AddBound(int64_t value) {
        if (!Bounds_) {
            Bounds_ = TBounds{value, value};
            return;
        }
        Bounds_->Min = std::min(Bounds_->Min, value);
        Bounds_->Max = std::max(Bounds_->Max, value);
    }

    void DropBounds() { BoundsUsable_ = false; }

    bool Overflowed() const { return Overflowed_; }
    size_t Size() const { return Hashes_.size(); }

private:
    friend class TRuntimeFilter;

    void Compact();

    struct TBounds {
        int64_t Min;
        int64_t Max;
    };

    size_t MaxKeys_;
    std::vector<uint64_t> Hashes_;
    std::optional<TBounds> Bounds_;
    bool BoundsUsable_ = true;
    bool Overflowed_ = false;
};

class TRuntimeFilter {
public:
    // Exact-set probes stop paying off beyond this size.
    static constexpr size_t DefaultMaxKeys = 1u << 18;

    explicit TRuntimeFilter(size_t maxKeys = DefaultMaxKeys) : MaxKeys_(maxKeys) {}

    TRuntimeFilter(const TRuntimeFilter&) = delete;
    TRuntimeFilter& operator=(const TRuntimeFilter&) = delete;

    TRuntimeFilterBuilder MakeBuilder() const {
        return TRuntimeFilterBuilder(MaxKeys_);
    }

    void Merge(TRuntimeFilterBuilder&& builder);

    // Publish only after every build-side task has merged.
    void Publish();

    bool Ready() const { return Ready_.load(std::memory_order_acquire); }
    // Bounds remain valid after exact-set overflow.
    bool ExactSetDisabled() const {
        return ExactSetDisabled_.load(std::memory_order_relaxed);
    }
    size_t KeyCount() const { return KeyCount_; }
    std::optional<std::pair<int64_t, int64_t>> Bounds() const;

    // Exact-set overflow does not disable independently collected bounds.
    bool MayContain(uint64_t hash) const;
    bool MayContainKey(int64_t value) const;

private:
    size_t MaxKeys_;
    std::mutex Mutex_;
    std::vector<uint64_t> Pending_;
    std::optional<TRuntimeFilterBuilder::TBounds> Bounds_;
    bool BoundsUsable_ = true;
    // May be observed before publication.
    std::atomic<bool> ExactSetDisabled_{false};
    size_t KeyCount_ = 0;
    std::vector<uint64_t> Table_;
    uint64_t Mask_ = 0;
    std::atomic<bool> Ready_{false};
};

} // namespace NQdb
