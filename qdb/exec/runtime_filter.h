#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace NQdb {

// Value-only contribution, suitable for transfer to a remote merger.
struct TRuntimeFilterPartial {
    std::vector<uint64_t> Hashes;
    std::vector<uint64_t> BloomWords;
    bool ExactSetOverflowed = false;
    uint64_t BuildRows = 0;
};

// Filters may return false positives, never false negatives. Builders are
// task-local; probes pass until the complete merged filter is published.
class TRuntimeFilterBuilder {
public:
    TRuntimeFilterBuilder(size_t maxKeys, size_t bloomBytes);
    void Add(uint64_t hash);
    bool Overflowed() const { return Overflowed_; }
    TRuntimeFilterPartial TakePartial() &&;

private:
    void Compact();

    size_t MaxKeys_;
    size_t BloomBytes_;
    std::vector<uint64_t> Hashes_;
    std::vector<uint64_t> BloomWords_;
    bool Overflowed_ = false;
    uint64_t BuildRows_ = 0;
};

// Owned by a scheduler task, not a worker thread: tasks may migrate.
// Policy and counters have one writer and are read after execution finishes.
struct alignas(64) TRuntimeFilterProbeState {
    uint64_t ProbeRows = 0;
    uint64_t RejectedRows = 0;
    uint64_t SkippedProbeRows = 0;
    uint64_t PausedProbeRows = 0;
    uint64_t SampleRows = 0;
    uint64_t SampleAccepted = 0;
    size_t SampleBatches = 0;
    size_t PauseBatches = 0;
    size_t PauseMultiplier = 0;
    std::vector<uint64_t> SelectedHashes;
    std::vector<uint8_t> SelectedOutput;

    void RecordBloomBatch(size_t rows, size_t accepted);
};

class IRuntimeFilterProducer {
public:
    virtual ~IRuntimeFilterProducer() = default;
    virtual TRuntimeFilterBuilder MakeBuilder() const = 0;
    // Once per lane at input EOF, before waiting for its output to drain.
    virtual void FinishProducer(TRuntimeFilterPartial&& partial) = 0;
};

class IRuntimeFilterProbe {
public:
    virtual ~IRuntimeFilterProbe() = default;
    // Called during graph construction, before any task starts.
    virtual std::shared_ptr<TRuntimeFilterProbeState> CreateProbeState();
    // Check before allocating masks or computing hashes. False passes the input
    // through unchanged. rows counts only rows in the incoming selection.
    virtual bool BeginBatch(TRuntimeFilterProbeState& state, size_t rows) const;
    virtual bool MayContain(uint64_t hash) const = 0;
    // Requires a successful BeginBatch. Returns the number of accepted rows.
    virtual size_t MayContainBatch(
        std::span<const uint64_t> hashes,
        std::span<uint8_t> output,
        TRuntimeFilterProbeState& state,
        std::span<const uint8_t> selection = {}) const;
};

struct TRuntimeFilterBinding {
    std::shared_ptr<IRuntimeFilterProducer> Producer;
    std::shared_ptr<IRuntimeFilterProbe> Probe;
};

// IDs are query-scoped; endpoints can also represent remote publication/probing.
class IRuntimeFilterBindingFactory {
public:
    virtual ~IRuntimeFilterBindingFactory() = default;
    virtual TRuntimeFilterBinding Create(uint32_t id, size_t producerCount) = 0;
    virtual TRuntimeFilterBinding CreateSized(
        uint32_t id, size_t producerCount, size_t bloomBytes) {
        (void)bloomBytes;
        return Create(id, producerCount);
    }
};

struct TRuntimeFilterStats {
    uint64_t BuildRows = 0;
    uint64_t ProbeRows = 0;
    uint64_t RejectedRows = 0;
    uint64_t SkippedProbeRows = 0;
    uint64_t PausedProbeRows = 0;
    size_t BloomBytes = 0;
    size_t BloomSetBits = 0;
    size_t ExactKeys = 0;
    double ReadyLatencyMs = 0.0;
    bool Ready = false;
    bool BloomDisabled = false;
};

class TRuntimeFilter final : public IRuntimeFilterProducer, public IRuntimeFilterProbe {
public:
    static constexpr size_t DefaultMaxKeys = 1u << 14;
    static constexpr size_t DefaultBloomBytes = 2u << 20;

    explicit TRuntimeFilter(size_t maxKeys = DefaultMaxKeys,
        size_t bloomBytes = DefaultBloomBytes);

    TRuntimeFilterBuilder MakeBuilder() const override {
        return TRuntimeFilterBuilder(MaxKeys_, BloomBytes_);
    }

    // Configure before execution. A standalone filter defaults to one producer.
    void SetProducerCount(size_t producers);
    void FinishProducer(TRuntimeFilterPartial&& partial) override;
    std::shared_ptr<TRuntimeFilterProbeState> CreateProbeState() override;
    bool BeginBatch(TRuntimeFilterProbeState& state, size_t rows) const override;
    bool MayContain(uint64_t hash) const override;
    size_t MayContainBatch(
        std::span<const uint64_t> hashes,
        std::span<uint8_t> output,
        TRuntimeFilterProbeState& state,
        std::span<const uint8_t> selection = {}) const override;

    bool Ready() const { return Ready_.load(std::memory_order_acquire); }
    bool UsesBloom() const { return Ready() && Data_.Kind == EKind::Bloom; }
    void StartExecutionClock();
    // Only while execution is quiescent; probe counters are task-local.
    TRuntimeFilterStats StatsSnapshot() const;

private:
    enum class EKind { PassAll, Exact, Bloom };
    // Written by the last producer, immutable after Ready_ is published.
    struct TPublishedData {
        EKind Kind = EKind::PassAll;
        std::vector<uint64_t> Table;
        std::vector<uint64_t> BloomWords;
        uint64_t Mask = 0;
        TRuntimeFilterStats Stats;
    };

    void Publish();
    void BuildPublishedData();
    size_t ProbeBloom(
        std::span<const uint64_t> hashes, std::span<uint8_t> output) const;

    size_t MaxKeys_;
    size_t BloomBytes_;
    std::vector<TRuntimeFilterPartial> Partials_;
    std::atomic<size_t> NextPartial_{0};
    std::atomic<size_t> PendingProducers_{0};
    TPublishedData Data_;
    std::atomic<bool> Ready_{false};
    std::vector<std::shared_ptr<TRuntimeFilterProbeState>> ProbeStates_;
    std::chrono::steady_clock::time_point ExecutionStart_;
    // Modified only during the initial scalar warmup, before the JIT threshold.
    mutable std::atomic<size_t> BloomProbeRows_{0};
};

class TLocalRuntimeFilterBindingFactory final : public IRuntimeFilterBindingFactory {
public:
    TRuntimeFilterBinding Create(uint32_t id, size_t producerCount) override;
    TRuntimeFilterBinding CreateSized(
        uint32_t id, size_t producerCount, size_t bloomBytes) override;
};

} // namespace NQdb
