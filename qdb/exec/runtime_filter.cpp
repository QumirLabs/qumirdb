#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/runtime_filter_vector.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace NQdb {
namespace {

// Reserve zero for empty slots.
uint64_t Storable(uint64_t hash) {
    return hash == 0 ? 1 : hash;
}

// One block has eight words, with one bit in each word. All producers use
// the same positions, so their contributions merge with a bitwise OR.
constexpr size_t BloomWordsPerBlock = 8;
constexpr size_t BloomBytesPerBlock = BloomWordsPerBlock * sizeof(uint64_t);

size_t NormalizeBloomBytes(size_t bytes) {
    return bytes < BloomBytesPerBlock ? 0 : std::bit_floor(bytes);
}

uint64_t Mix(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

size_t BloomBlock(const std::vector<uint64_t>& words, uint64_t hash) {
    return static_cast<size_t>(Mix(hash) & (words.size() / BloomWordsPerBlock - 1))
        * BloomWordsPerBlock;
}

void BloomAdd(std::vector<uint64_t>& words, uint64_t hash) {
    if (words.empty()) {
        return;
    }
    const size_t block = BloomBlock(words, hash);
    const uint64_t bits = Mix(hash ^ 0x9e3779b97f4a7c15ULL);
    for (size_t i = 0; i < BloomWordsPerBlock; ++i) {
        words[block + i] |= uint64_t{1} << ((bits >> (i * 8)) & 63);
    }
}

bool BloomMayContain(const std::vector<uint64_t>& words, uint64_t hash) {
    const size_t block = BloomBlock(words, hash);
    const uint64_t bits = Mix(hash ^ 0x9e3779b97f4a7c15ULL);
    for (size_t i = 0; i < BloomWordsPerBlock; ++i) {
        if ((words[block + i] & (uint64_t{1} << ((bits >> (i * 8)) & 63))) == 0) {
            return false;
        }
    }
    return true;
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

size_t IRuntimeFilterProbe::MayContainBatch(
    std::span<const uint64_t> hashes,
    std::span<uint8_t> output,
    TRuntimeFilterProbeState& state,
    std::span<const uint8_t> selection) const
{
    CheckBatchSizes(hashes.size(), output.size(), selection.size());
    size_t accepted = 0;
    for (size_t i = 0; i < hashes.size(); ++i) {
        const bool keep = (selection.empty() || selection[i]) && MayContain(hashes[i]);
        output[i] = keep ? 0xff : 0;
        accepted += keep;
    }
    state.RejectedRows += SelectedRows(hashes.size(), selection) - accepted;
    return accepted;
}

TRuntimeFilterBuilder::TRuntimeFilterBuilder(size_t maxKeys, size_t bloomBytes)
    : MaxKeys_(maxKeys)
    , BloomBytes_(NormalizeBloomBytes(bloomBytes))
{}

void TRuntimeFilterBuilder::Add(uint64_t hash) {
    ++BuildRows_;
    if (Overflowed_) {
        BloomAdd(BloomWords_, hash);
        return;
    }
    Hashes_.push_back(hash);
    // Delay compaction to avoid sorting every duplicate at the cap.
    if (Hashes_.size() >= MaxKeys_ * 2) {
        Compact();
    }
}

void TRuntimeFilterBuilder::Compact() {
    std::ranges::sort(Hashes_);
    const auto duplicates = std::ranges::unique(Hashes_);
    Hashes_.erase(duplicates.begin(), duplicates.end());
    if (Hashes_.size() > MaxKeys_) {
        Overflowed_ = true;
        BloomWords_.assign(BloomBytes_ / sizeof(uint64_t), 0);
        for (uint64_t hash : Hashes_) {
            BloomAdd(BloomWords_, hash);
        }
        std::vector<uint64_t>().swap(Hashes_);
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

TRuntimeFilter::TRuntimeFilter(size_t maxKeys, size_t bloomBytes)
    : MaxKeys_(maxKeys)
    , BloomBytes_(NormalizeBloomBytes(bloomBytes))
{
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
            for (size_t i = 0; i < partial.BloomWords.size(); ++i) {
                Data_.BloomWords[i] |= partial.BloomWords[i];
            }
            for (uint64_t hash : partial.Hashes) {
                BloomAdd(Data_.BloomWords, hash);
            }
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
        std::ranges::sort(keys);
        const auto duplicates = std::ranges::unique(keys);
        keys.erase(duplicates.begin(), duplicates.end());
        if (keys.size() <= MaxKeys_) {
            Data_.Kind = EKind::Exact;
            Data_.Stats.ExactKeys = keys.size();
            const size_t capacity = std::bit_ceil(std::max<size_t>(8, keys.size() * 2));
            Data_.Table.assign(capacity, 0);
            Data_.Mask = capacity - 1;
            for (uint64_t hash : keys) {
                const uint64_t stored = Storable(hash);
                uint64_t slot = hash & Data_.Mask;
                while (Data_.Table[slot] != 0 && Data_.Table[slot] != stored) {
                    slot = (slot + 1) & Data_.Mask;
                }
                Data_.Table[slot] = stored;
            }
            return;
        }
        if (!BloomBytes_) {
            Data_.Stats.BloomDisabled = true;
            return;
        }
        Data_.BloomWords.assign(BloomBytes_ / sizeof(uint64_t), 0);
        for (uint64_t hash : keys) {
            BloomAdd(Data_.BloomWords, hash);
        }
    }

    Data_.Kind = EKind::Bloom;
    Data_.Stats.BloomBytes = Data_.BloomWords.size() * sizeof(uint64_t);
    for (uint64_t word : Data_.BloomWords) {
        Data_.Stats.BloomSetBits += std::popcount(word);
    }
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
    ProbeStates_.push_back(state);
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

bool TRuntimeFilter::MayContain(uint64_t hash) const {
    if (!Ready() || Data_.Kind == EKind::PassAll) {
        return true;
    }
    if (Data_.Kind == EKind::Bloom) {
        return BloomMayContain(Data_.BloomWords, hash);
    }
    const uint64_t stored = Storable(hash);
    uint64_t slot = hash & Data_.Mask;
    while (Data_.Table[slot] != 0 && Data_.Table[slot] != stored) {
        slot = (slot + 1) & Data_.Mask;
    }
    return Data_.Table[slot] == stored;
}

size_t TRuntimeFilter::ProbeBloom(
    std::span<const uint64_t> hashes, std::span<uint8_t> output) const
{
    const size_t vectorized = ProbeBloomVectorBatch(Data_.BloomWords, hashes, output);
    size_t accepted = static_cast<size_t>(std::count_if(
        output.begin(), output.begin() + vectorized, [](uint8_t value) { return value != 0; }));
    for (size_t i = vectorized; i < hashes.size(); ++i) {
        const bool keep = BloomMayContain(Data_.BloomWords, hashes[i]);
        output[i] = keep ? 0xff : 0;
        accepted += keep;
    }
    return accepted;
}

size_t TRuntimeFilter::MayContainBatch(
    std::span<const uint64_t> hashes,
    std::span<uint8_t> output,
    TRuntimeFilterProbeState& state,
    std::span<const uint8_t> selection) const
{
    CheckBatchSizes(hashes.size(), output.size(), selection.size());
    const size_t rows = SelectedRows(hashes.size(), selection);
    size_t accepted = 0;
    if (Data_.Kind == EKind::Bloom) {
        // Stop modifying the shared warmup counter once compilation is allowed.
        constexpr size_t VectorCompileThreshold = 1'000'000;
        size_t seen = BloomProbeRows_.load(std::memory_order_relaxed);
        if (seen < VectorCompileThreshold) {
            seen = BloomProbeRows_.fetch_add(rows, std::memory_order_relaxed);
        }
        const bool vectorize = seen >= VectorCompileThreshold
            || rows >= VectorCompileThreshold - seen;
        if (vectorize && rows == hashes.size()) {
            accepted = ProbeBloom(hashes, output);
        } else if (vectorize && rows >= 8192) {
            // Compact sparse input masks so the vector kernel also ignores
            // rows already rejected upstream. Scratch space belongs to a lane.
            state.SelectedHashes.clear();
            for (size_t i = 0; i < hashes.size(); ++i) {
                if (selection[i]) {
                    state.SelectedHashes.push_back(hashes[i]);
                }
            }
            state.SelectedOutput.resize(rows);
            accepted = ProbeBloom(state.SelectedHashes, state.SelectedOutput);
            size_t selected = 0;
            for (size_t i = 0; i < hashes.size(); ++i) {
                output[i] = selection[i] ? state.SelectedOutput[selected++] : 0;
            }
        } else {
            // Without SIMD, probing the mask directly avoids scratch copies.
            for (size_t i = 0; i < hashes.size(); ++i) {
                const bool keep = (selection.empty() || selection[i])
                    && BloomMayContain(Data_.BloomWords, hashes[i]);
                output[i] = keep ? 0xff : 0;
                accepted += keep;
            }
        }
        state.RecordBloomBatch(rows, accepted);
    } else {
        for (size_t i = 0; i < hashes.size(); ++i) {
            if (!selection.empty() && !selection[i]) {
                output[i] = 0;
                continue;
            }
            const uint64_t stored = Storable(hashes[i]);
            uint64_t slot = hashes[i] & Data_.Mask;
            while (Data_.Table[slot] != 0 && Data_.Table[slot] != stored) {
                slot = (slot + 1) & Data_.Mask;
            }
            const bool keep = Data_.Table[slot] == stored;
            output[i] = keep ? 0xff : 0;
            accepted += keep;
        }
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

TRuntimeFilterBinding TLocalRuntimeFilterBindingFactory::Create(
    uint32_t id, size_t producerCount)
{
    return CreateSized(id, producerCount, TRuntimeFilter::DefaultBloomBytes);
}

TRuntimeFilterBinding TLocalRuntimeFilterBindingFactory::CreateSized(
    uint32_t id, size_t producerCount, size_t bloomBytes)
{
    (void)id;
    auto filter = std::make_shared<TRuntimeFilter>(TRuntimeFilter::DefaultMaxKeys, bloomBytes);
    filter->SetProducerCount(producerCount);
    return TRuntimeFilterBinding{.Producer = filter, .Probe = std::move(filter)};
}

} // namespace NQdb
