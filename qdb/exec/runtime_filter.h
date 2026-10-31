#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace NQdb {

struct TRuntimeContext;
class TThreadExecutionContext;

// The runtime-filter kernel (qdb/kernel/join/runtime_filter.oz) implements
// every operation on filter contents; these are its entry points, in
// TKernelSlot::Fns order. C++ provides the storage and calls it once per batch.
inline constexpr std::array<const char*, 3> RuntimeFilterKernelEntrypoints = {
    "rf_build", "rf_publish", "rf_probe"};

// Mirrors RuntimeFilterBuild in the kernel, which documents the fields.
struct TRuntimeFilterBuildState {
    enum class EMode : int64_t { Collecting, Deduplicating, Overflowing, Bloom };
    enum class EWant : int64_t { Nothing, CompactionStorage, BloomStorage };

    int64_t MaxKeys = 0;
    EMode Mode = EMode::Collecting;
    EWant Want = EWant::Nothing;
    uint64_t* Keys = nullptr;
    int64_t KeyCount = 0;
    uint8_t* SetCtrl = nullptr;
    uint64_t* SetSlots = nullptr;
    int64_t SetCapacity = 0;
    uint32_t* SortOrder = nullptr;
    uint64_t* SortKeys = nullptr;
    uint64_t* Bloom = nullptr;
    int64_t BloomWords = -1;
};

// Value-only contribution, suitable for transfer to a remote merger.
struct TRuntimeFilterPartial {
    std::vector<uint64_t> Hashes;
    std::vector<uint64_t> BloomWords;
    bool ExactSetOverflowed = false;
    uint64_t BuildRows = 0;
};

// Filters may return false positives, never false negatives. Builders are
// task-local; consumer tasks wait for the complete merged filter.
// runtimeContext must hold the runtime-filter kernel.
class TRuntimeFilterBuilder {
public:
    TRuntimeFilterBuilder(std::shared_ptr<TRuntimeContext> runtimeContext,
        size_t maxKeys, size_t bloomBytes);
    // An empty selection selects every row.
    void Add(
        TThreadExecutionContext& context,
        std::span<const uint64_t> hashes,
        std::span<const uint8_t> selection = {});
    bool Overflowed() const { return State_.Mode == TRuntimeFilterBuildState::EMode::Bloom; }
    TRuntimeFilterPartial TakePartial() &&;

private:
    void ProvideStorage(size_t rows);

    std::shared_ptr<TRuntimeContext> RuntimeContext_;
    size_t BloomBytes_;
    TRuntimeFilterBuildState State_;
    std::vector<uint64_t> Keys_;
    std::vector<uint8_t> SetCtrl_;
    std::vector<uint64_t> SetSlots_;
    std::vector<uint32_t> SortOrder_;
    std::vector<uint64_t> SortKeys_;
    std::vector<uint64_t> BloomWords_;
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
    virtual void FinishProducer(
        TThreadExecutionContext& context, TRuntimeFilterPartial&& partial) = 0;
};

class IRuntimeFilterProbe {
public:
    virtual ~IRuntimeFilterProbe() = default;
    // Auxiliary inputs wait for a complete filter or PassAll publication.
    virtual bool Ready() const = 0;
    // Created lazily by each consumer task after the filter is published.
    virtual std::shared_ptr<TRuntimeFilterProbeState> CreateProbeState();
    // Check before allocating masks or computing hashes. False passes the input
    // through unchanged. rows counts only rows in the incoming selection.
    virtual bool BeginBatch(TRuntimeFilterProbeState& state, size_t rows) const;
    // Requires a successful BeginBatch. Returns the number of accepted rows.
    virtual size_t MayContainBatch(
        TThreadExecutionContext& context,
        std::span<const uint64_t> hashes,
        std::span<uint8_t> output,
        TRuntimeFilterProbeState& state,
        std::span<const uint8_t> selection = {}) const = 0;
};

struct TRuntimeFilterBinding {
    std::shared_ptr<IRuntimeFilterProducer> Producer;
    std::shared_ptr<IRuntimeFilterProbe> Probe;
};

// IDs are query-scoped; endpoints can also represent remote publication/probing.
// runtimeContext belongs to the query and holds its runtime-filter kernel.
class IRuntimeFilterBindingFactory {
public:
    virtual ~IRuntimeFilterBindingFactory() = default;
    virtual TRuntimeFilterBinding Create(
        uint32_t id, size_t producerCount, size_t bloomBytes,
        std::shared_ptr<TRuntimeContext> runtimeContext) = 0;
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

    explicit TRuntimeFilter(std::shared_ptr<TRuntimeContext> runtimeContext,
        size_t maxKeys = DefaultMaxKeys,
        size_t bloomBytes = DefaultBloomBytes);

    TRuntimeFilterBuilder MakeBuilder() const override {
        return TRuntimeFilterBuilder(RuntimeContext_, MaxKeys_, BloomBytes_);
    }

    // Configure before execution. A standalone filter defaults to one producer.
    void SetProducerCount(size_t producers);
    void FinishProducer(
        TThreadExecutionContext& context, TRuntimeFilterPartial&& partial) override;
    std::shared_ptr<TRuntimeFilterProbeState> CreateProbeState() override;
    bool BeginBatch(TRuntimeFilterProbeState& state, size_t rows) const override;
    size_t MayContainBatch(
        TThreadExecutionContext& context,
        std::span<const uint64_t> hashes,
        std::span<uint8_t> output,
        TRuntimeFilterProbeState& state,
        std::span<const uint8_t> selection = {}) const override;

    bool Ready() const override { return Ready_.load(std::memory_order_acquire); }
    bool UsesBloom() const { return Ready() && Data_.Kind == EKind::Bloom; }
    void StartExecutionClock();
    // Only while execution is quiescent; probe counters are task-local.
    TRuntimeFilterStats StatsSnapshot() const;

private:
    // Values are the kernel's filter kinds.
    enum class EKind : int64_t { PassAll, Exact, Bloom };
    // Written by the last producer, immutable after Ready_ is published.
    struct TPublishedData {
        EKind Kind = EKind::PassAll;
        // The exact table or the Bloom words.
        std::vector<uint64_t> Words;
        TRuntimeFilterStats Stats;
    };

    void Publish(TThreadExecutionContext& context);
    void BuildPublishedData(TThreadExecutionContext& context);

    std::shared_ptr<TRuntimeContext> RuntimeContext_;
    size_t MaxKeys_;
    size_t BloomBytes_;
    std::vector<TRuntimeFilterPartial> Partials_;
    std::atomic<size_t> NextPartial_{0};
    std::atomic<size_t> PendingProducers_{0};
    TPublishedData Data_;
    std::atomic<bool> Ready_{false};
    std::mutex ProbeStatesMutex_;
    std::vector<std::shared_ptr<TRuntimeFilterProbeState>> ProbeStates_;
    std::chrono::steady_clock::time_point ExecutionStart_;
};

class TLocalRuntimeFilterBindingFactory final : public IRuntimeFilterBindingFactory {
public:
    TRuntimeFilterBinding Create(
        uint32_t id, size_t producerCount, size_t bloomBytes,
        std::shared_ptr<TRuntimeContext> runtimeContext) override;
};

} // namespace NQdb
