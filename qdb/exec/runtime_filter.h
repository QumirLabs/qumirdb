#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace NQdb {

// One producer's contribution. This contains values rather than pointers into
// a scheduler task, so a remote publisher can encode and send it to a merger.
struct TRuntimeFilterPartial {
    std::vector<uint64_t> Hashes;
    std::optional<std::pair<int64_t, int64_t>> Bounds;
    bool BoundsUsable = true;
    bool ExactSetOverflowed = false;
};

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
    TRuntimeFilterPartial TakePartial() &&;

private:
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

class IRuntimeFilterProducer {
public:
    virtual ~IRuntimeFilterProducer() = default;
    virtual TRuntimeFilterBuilder MakeBuilder() const = 0;
    // Called once per lane after its final build-side row has been processed.
    virtual void FinishProducer(TRuntimeFilterPartial&& partial) = 0;
};

class IRuntimeFilterProbe {
public:
    virtual ~IRuntimeFilterProbe() = default;
    // A missing or incomplete filter must pass every hash.
    virtual bool MayContain(uint64_t hash) const = 0;
};

struct TRuntimeFilterBinding {
    std::shared_ptr<IRuntimeFilterProducer> Producer;
    std::shared_ptr<IRuntimeFilterProbe> Probe;
};

// The filter id is scoped to one query. A distributed implementation can bind
// it to remote publication/subscription endpoints instead of shared memory.
class IRuntimeFilterBindingFactory {
public:
    virtual ~IRuntimeFilterBindingFactory() = default;
    virtual TRuntimeFilterBinding Create(uint32_t id, size_t producerCount) = 0;
};

class TRuntimeFilter final : public IRuntimeFilterProducer, public IRuntimeFilterProbe {
public:
    // Exact-set probes stop paying off beyond this size.
    static constexpr size_t DefaultMaxKeys = 1u << 18;

    explicit TRuntimeFilter(size_t maxKeys = DefaultMaxKeys) : MaxKeys_(maxKeys) {}

    TRuntimeFilter(const TRuntimeFilter&) = delete;
    TRuntimeFilter& operator=(const TRuntimeFilter&) = delete;

    TRuntimeFilterBuilder MakeBuilder() const override {
        return TRuntimeFilterBuilder(MaxKeys_);
    }

    void SetProducerCount(size_t producers);

    // Merges a single producer's builder into the filter
    // Calls Publish() when the last producer finishes
    void FinishProducer(TRuntimeFilterBuilder&& builder);
    void FinishProducer(TRuntimeFilterPartial&& partial) override;

    void Merge(TRuntimeFilterBuilder&& builder); // effectively a private helper for FinishProducer
    void Merge(TRuntimeFilterPartial&& partial);

    // Publish only after every build-side task has merged.
    void Publish(); // effectively a private helper for FinishProducer

    bool Ready() const { return Ready_.load(std::memory_order_acquire); }
    // Bounds remain valid after exact-set overflow.
    bool ExactSetDisabled() const {
        return ExactSetDisabled_.load(std::memory_order_relaxed);
    }
    size_t KeyCount() const { return KeyCount_; }
    std::optional<std::pair<int64_t, int64_t>> Bounds() const;

    // Exact-set overflow does not disable independently collected bounds.
    bool MayContain(uint64_t hash) const override;
    bool MayContainKey(int64_t value) const;

private:
    size_t MaxKeys_;
    std::mutex Mutex_;
    std::vector<uint64_t> Pending_;
    std::optional<std::pair<int64_t, int64_t>> Bounds_;
    bool BoundsUsable_ = true;
    // May be observed before publication.
    std::atomic<bool> ExactSetDisabled_{false};
    size_t KeyCount_ = 0;
    std::vector<uint64_t> Table_;
    uint64_t Mask_ = 0;
    std::atomic<bool> Ready_{false};
    std::atomic<size_t> PendingProducers_{0};
};

class TLocalRuntimeFilterBindingFactory final : public IRuntimeFilterBindingFactory {
public:
    TRuntimeFilterBinding Create(uint32_t id, size_t producerCount) override;
};

} // namespace NQdb
