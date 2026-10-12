#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/runtime_context.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace NQdb {
namespace {

// A Bloom filter is a power-of-two number of 64-byte blocks.
constexpr size_t BloomBytesPerBlock = 8 * sizeof(uint64_t);

// Smaller batches use the scalar Bloom probe.
constexpr size_t VectorProbeRows = 8192;

size_t NormalizeBloomBytes(size_t bytes) {
    return bytes < BloomBytesPerBlock ? 0 : std::bit_floor(bytes);
}

TRuntimeFilterKernels Kernels(const std::shared_ptr<TRuntimeContext>& context) {
    return TRuntimeFilterKernels(*context->RuntimeFilterKernels);
}

void CheckKernels(const std::shared_ptr<TRuntimeContext>& context) {
    if (!context || !context->RuntimeFilterKernels) {
        throw std::invalid_argument("runtime filter requires the runtime-filter kernel");
    }
}

size_t SelectedRows(size_t rows, std::span<const uint8_t> selection) {
    return selection.empty() ? rows : static_cast<size_t>(std::count_if(
        selection.begin(), selection.end(), [](uint8_t value) { return value != 0; }));
}

void CheckBatchSizes(size_t rows, size_t output, size_t selection) {
    if (rows != output || (selection != 0 && selection != rows)) {
        throw std::invalid_argument("runtime filter batch size mismatch");
    }
}

} // namespace

std::shared_ptr<TRuntimeFilterProbeState> IRuntimeFilterProbe::CreateProbeState() {
    return std::make_shared<TRuntimeFilterProbeState>();
}

bool IRuntimeFilterProbe::BeginBatch(TRuntimeFilterProbeState& state, size_t rows) const {
    state.ProbeRows += rows;
    return rows != 0;
}

TRuntimeFilterBuilder::TRuntimeFilterBuilder(
    std::shared_ptr<TRuntimeContext> runtimeContext, size_t maxKeys, size_t bloomBytes)
    : RuntimeContext_(std::move(runtimeContext))
    , MaxKeys_(maxKeys)
    , BloomBytes_(NormalizeBloomBytes(bloomBytes))
{
    CheckKernels(RuntimeContext_);
}

void TRuntimeFilterBuilder::Add(
    std::span<const uint64_t> hashes, std::span<const uint8_t> selection)
{
    CheckBatchSizes(hashes.size(), hashes.size(), selection.size());
    BuildRows_ += SelectedRows(hashes.size(), selection);
    const auto kernels = Kernels(RuntimeContext_);
    for (size_t row = 0; row < hashes.size();) {
        const auto rest = hashes.subspan(row);
        const auto restSelection = selection.empty() ? selection : selection.subspan(row);
        if (Overflowed_) {
            kernels.BloomAdd(BloomWords_, rest, restSelection);
            return;
        }
        if (Deduplicating_) {
            row += kernels.AppendDistinctKeys(Seen_, Hashes_, MaxKeys_, rest, restSelection);
            if (Hashes_.size() > MaxKeys_) {
                OverflowToBloom();
            }
            continue;
        }
        row += kernels.AppendKeys(Hashes_, MaxKeys_ * 2, rest, restSelection);
        // One compaction distinguishes mostly unique from duplicate-heavy input.
        // Afterwards the exact set deduplicates incrementally.
        if (Hashes_.size() >= MaxKeys_ * 2) {
            Compact();
        }
    }
}

void TRuntimeFilterBuilder::OverflowToBloom() {
    Overflowed_ = true;
    BloomWords_.assign(BloomBytes_ / sizeof(uint64_t), 0);
    Kernels(RuntimeContext_).BloomAdd(BloomWords_, Hashes_);
    std::vector<uint64_t>().swap(Hashes_);
    Seen_ = {};
    Deduplicating_ = false;
}

void TRuntimeFilterBuilder::Compact() {
    const auto kernels = Kernels(RuntimeContext_);
    kernels.SortUnique(Hashes_);
    if (Hashes_.size() > MaxKeys_) {
        OverflowToBloom();
    } else {
        kernels.FillSet(Seen_, MaxKeys_, Hashes_);
        Deduplicating_ = true;
    }
}

TRuntimeFilterPartial TRuntimeFilterBuilder::TakePartial() && {
    return TRuntimeFilterPartial{
        .Hashes = std::move(Hashes_),
        .BloomWords = std::move(BloomWords_),
        .ExactSetOverflowed = Overflowed_,
        .BuildRows = BuildRows_,
    };
}

TRuntimeFilter::TRuntimeFilter(std::shared_ptr<TRuntimeContext> runtimeContext,
    size_t maxKeys, size_t bloomBytes)
    : RuntimeContext_(std::move(runtimeContext))
    , MaxKeys_(maxKeys)
    , BloomBytes_(NormalizeBloomBytes(bloomBytes))
{
    CheckKernels(RuntimeContext_);
    SetProducerCount(1);
}

void TRuntimeFilter::SetProducerCount(size_t producers) {
    if (!producers || NextPartial_.load(std::memory_order_relaxed) != 0 || Ready()) {
        throw std::invalid_argument("runtime filter producers must be configured before execution");
    }
    Partials_.resize(producers);
    PendingProducers_.store(producers, std::memory_order_relaxed);
}

void TRuntimeFilter::StartExecutionClock() {
    ExecutionStart_ = std::chrono::steady_clock::now();
}

void TRuntimeFilter::FinishProducer(TRuntimeFilterPartial&& partial) {
    const size_t slot = NextPartial_.fetch_add(1, std::memory_order_relaxed);
    // Each producer writes its own slot. The acq_rel decrement chain makes all
    // contributions visible to the last producer, which alone builds Data_.
    assert(slot < Partials_.size());
    Partials_[slot] = std::move(partial);
    if (PendingProducers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        Publish();
    }
}

void TRuntimeFilter::BuildPublishedData() {
    const auto kernels = Kernels(RuntimeContext_);
    bool incomplete = false;
    size_t exactHashes = 0;
    for (const auto& partial : Partials_) {
        Data_.Stats.BuildRows += partial.BuildRows;
        exactHashes += partial.Hashes.size();
        incomplete |= partial.ExactSetOverflowed && partial.BloomWords.empty();
        incomplete |= !partial.BloomWords.empty()
            && partial.BloomWords.size() != BloomBytes_ / sizeof(uint64_t);
    }
    if (incomplete) {
        // A missing contribution or a transport/configuration mismatch must
        // never reject a key.
        Data_.Stats.BloomDisabled = true;
        return;
    }

    // Reuse one producer's Bloom allocation instead of allocating and OR-ing
    // an initially empty global filter.
    for (auto& partial : Partials_) {
        if (!partial.BloomWords.empty()) {
            Data_.BloomWords.swap(partial.BloomWords);
            break;
        }
    }
    if (!Data_.BloomWords.empty()) {
        for (const auto& partial : Partials_) {
            kernels.BloomOr(Data_.BloomWords, partial.BloomWords);
            kernels.BloomAdd(Data_.BloomWords, partial.Hashes);
        }
    } else {
        std::vector<uint64_t> keys;
        for (auto& partial : Partials_) {
            if (!partial.Hashes.empty()) {
                keys.swap(partial.Hashes);
                break;
            }
        }
        keys.reserve(exactHashes);
        for (const auto& partial : Partials_) {
            keys.insert(keys.end(), partial.Hashes.begin(), partial.Hashes.end());
        }
        // Sort once, after all producers finish, including duplicates across
        // lanes. Raw hashes are retained until Bloom promotion (including 0).
        kernels.SortUnique(keys);
        if (keys.size() <= MaxKeys_) {
            Data_.Kind = EKind::Exact;
            Data_.Stats.ExactKeys = keys.size();
            Data_.Table.assign(std::bit_ceil(std::max<size_t>(8, keys.size() * 2)), 0);
            kernels.ExactBuild(Data_.Table, keys);
            return;
        }
        if (!BloomBytes_) {
            Data_.Stats.BloomDisabled = true;
            return;
        }
        Data_.BloomWords.assign(BloomBytes_ / sizeof(uint64_t), 0);
        kernels.BloomAdd(Data_.BloomWords, keys);
    }

    Data_.Kind = EKind::Bloom;
    Data_.Stats.BloomBytes = Data_.BloomWords.size() * sizeof(uint64_t);
    Data_.Stats.BloomSetBits = kernels.BloomSetBits(Data_.BloomWords);
    // At this density the false-positive rate approaches 50%, making the
    // extra probe unlikely to pay for itself.
    if (Data_.Stats.BloomSetBits * 10 >= Data_.BloomWords.size() * 64 * 9) {
        Data_.Kind = EKind::PassAll;
        Data_.Stats.BloomDisabled = true;
        std::vector<uint64_t>().swap(Data_.BloomWords);
    }
}

void TRuntimeFilter::Publish() {
    BuildPublishedData();
    std::vector<TRuntimeFilterPartial>().swap(Partials_);
    if (ExecutionStart_ != std::chrono::steady_clock::time_point{}) {
        Data_.Stats.ReadyLatencyMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ExecutionStart_).count();
    }
    Data_.Stats.Ready = true;
    Ready_.store(true, std::memory_order_release);
}

std::shared_ptr<TRuntimeFilterProbeState> TRuntimeFilter::CreateProbeState() {
    auto state = IRuntimeFilterProbe::CreateProbeState();
    {
        std::lock_guard lock(ProbeStatesMutex_);
        ProbeStates_.push_back(state);
    }
    return state;
}

bool TRuntimeFilter::BeginBatch(TRuntimeFilterProbeState& state, size_t rows) const {
    state.ProbeRows += rows;
    if (!rows) {
        return false;
    }
    if (!Ready() || Data_.Kind == EKind::PassAll) {
        state.SkippedProbeRows += rows;
        return false;
    }
    if (Data_.Kind == EKind::Bloom && state.PauseBatches) {
        --state.PauseBatches;
        state.SkippedProbeRows += rows;
        state.PausedProbeRows += rows;
        return false;
    }
    return true;
}

size_t TRuntimeFilter::MayContainBatch(
    std::span<const uint64_t> hashes,
    std::span<uint8_t> output,
    TRuntimeFilterProbeState& state,
    std::span<const uint8_t> selection) const
{
    CheckBatchSizes(hashes.size(), output.size(), selection.size());
    const size_t rows = SelectedRows(hashes.size(), selection);
    const auto kernels = Kernels(RuntimeContext_);
    size_t accepted = 0;
    if (Data_.Kind == EKind::Bloom) {
        // The vector probe packs sparse input masks so that it also ignores
        // rows already rejected upstream. Scratch space belongs to a lane.
        accepted = rows >= VectorProbeRows
            ? kernels.BloomProbeVector(Data_.BloomWords, hashes, selection, output,
                state.SelectedHashes, state.SelectedOutput)
            : kernels.BloomProbeScalar(Data_.BloomWords, hashes, selection, output);
        state.RecordBloomBatch(rows, accepted);
    } else {
        accepted = kernels.ExactProbe(Data_.Table, hashes, selection, output);
    }
    state.RejectedRows += rows - accepted;
    return accepted;
}

void TRuntimeFilterProbeState::RecordBloomBatch(size_t rows, size_t accepted) {
    SampleRows += rows;
    SampleAccepted += accepted;
    ++SampleBatches;
    if (SampleBatches < 6 || SampleRows < 32768) {
        return;
    }
    if (SampleAccepted * 2 >= SampleRows) {
        PauseMultiplier = std::min<size_t>(PauseMultiplier + 1, 16);
        PauseBatches = PauseMultiplier * 4;
    } else {
        PauseMultiplier = 0;
    }
    SampleRows = 0;
    SampleAccepted = 0;
    SampleBatches = 0;
}

TRuntimeFilterStats TRuntimeFilter::StatsSnapshot() const {
    TRuntimeFilterStats stats = Ready() ? Data_.Stats : TRuntimeFilterStats{};
    for (const auto& state : ProbeStates_) {
        stats.ProbeRows += state->ProbeRows;
        stats.RejectedRows += state->RejectedRows;
        stats.SkippedProbeRows += state->SkippedProbeRows;
        stats.PausedProbeRows += state->PausedProbeRows;
    }
    return stats;
}

TLocalRuntimeFilterBindingFactory::TLocalRuntimeFilterBindingFactory(
    std::shared_ptr<TRuntimeContext> runtimeContext)
    : RuntimeContext_(std::move(runtimeContext))
{}

TRuntimeFilterBinding TLocalRuntimeFilterBindingFactory::Create(
    uint32_t id, size_t producerCount, size_t bloomBytes)
{
    (void)id;
    auto filter = std::make_shared<TRuntimeFilter>(
        RuntimeContext_, TRuntimeFilter::DefaultMaxKeys, bloomBytes);
    filter->SetProducerCount(producerCount);
    return TRuntimeFilterBinding{.Producer = filter, .Probe = std::move(filter)};
}

} // namespace NQdb
