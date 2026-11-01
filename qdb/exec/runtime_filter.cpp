#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/kernel/generated.h>

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

// Mirror RuntimeFilterPartial, RuntimeFilterPublish and RuntimeFilterData in
// the kernel, which documents the fields.
struct TPartialView {
    const uint64_t* Keys;
    int64_t KeyCount;
    const uint64_t* Bloom;
    int64_t BloomWords;
};

struct TPublishState {
    int64_t MaxKeys = 0;
    uint64_t* Keys = nullptr;
    uint32_t* SortOrder = nullptr;
    uint64_t* SortKeys = nullptr;
    uint64_t* Table = nullptr;
    uint64_t* Bloom = nullptr;
    int64_t BloomWords = 0;
    int64_t BloomSeeded = 0;
    int64_t Kind = 0;
    int64_t ExactKeys = 0;
    int64_t TableSize = 0;
    int64_t BloomSetBits = 0;
};

struct TFilterData {
    int64_t Kind;
    const uint64_t* Words;
    int64_t WordCount;
};

static_assert(sizeof(TRuntimeFilterBuildState) == 12 * sizeof(int64_t));
static_assert(sizeof(TPartialView) == 4 * sizeof(int64_t));
static_assert(sizeof(TPublishState) == 12 * sizeof(int64_t));
static_assert(sizeof(TFilterData) == 3 * sizeof(int64_t));

using EMode = TRuntimeFilterBuildState::EMode;
using EWant = TRuntimeFilterBuildState::EWant;

// Parallel to RuntimeFilterKernelEntrypoints.
enum EEntry : size_t { Build, Publish, Probe };

using TBuild = int64_t (*)(TRuntimeFilterBuildState*, const uint64_t*, const uint8_t*, int64_t);
using TPublish = void (*)(TPublishState*, const TPartialView*, int64_t);
using TProbe = int64_t (*)(const TFilterData*, const uint64_t*, const uint8_t*, uint8_t*,
    int64_t, int64_t, uint64_t*, uint8_t*);

template <typename T, typename... TArgs>
auto CallEntry(const TRuntimeContext& context, EEntry entry, TArgs... args) {
    return context.RuntimeFilterKernels->Call<T>(entry, args...);
}

void CheckKernels(const std::shared_ptr<TRuntimeContext>& context) {
    if (!context || !context->RuntimeFilterKernels) {
        throw std::invalid_argument("runtime filter requires the runtime-filter kernel");
    }
}

// Scratch for sorting count keys: two index arrays and the radix counters.
size_t SortOrderSize(size_t count) {
    return count * 2 + 256;
}

template <typename T>
void Free(std::vector<T>& storage) {
    std::vector<T>().swap(storage);
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
    , BloomBytes_(NormalizeBloomBytes(bloomBytes))
{
    CheckKernels(RuntimeContext_);
    State_.MaxKeys = static_cast<int64_t>(maxKeys);
}

void TRuntimeFilterBuilder::Add(
    std::span<const uint64_t> hashes, std::span<const uint8_t> selection)
{
    CheckBatchSizes(hashes.size(), hashes.size(), selection.size());
    BuildRows_ += SelectedRows(hashes.size(), selection);
    size_t row = 0;
    // The kernel stops only to ask for storage, and continues from that row.
    do {
        ProvideStorage(hashes.size() - row);
        row += static_cast<size_t>(CallEntry<TBuild>(*RuntimeContext_, EEntry::Build,
            &State_, hashes.data() + row,
            selection.empty() ? nullptr : selection.data() + row,
            static_cast<int64_t>(hashes.size() - row)));
    } while (State_.Want != EWant::Nothing);

    if (State_.Mode != EMode::Collecting) {
        Free(SortOrder_);
        Free(SortKeys_);
    }
    if (State_.Mode == EMode::Bloom) {
        Free(Keys_);
        Free(SetCtrl_);
        Free(SetSlots_);
    }
}

void TRuntimeFilterBuilder::ProvideStorage(size_t rows) {
    if (State_.Want == EWant::CompactionStorage) {
        const auto keys = static_cast<size_t>(State_.KeyCount);
        SortOrder_.resize(SortOrderSize(keys));
        SortKeys_.resize(keys);
        // Keeps the set at most half full, in whole swiss groups.
        const size_t capacity = std::bit_ceil(
            std::max<size_t>(16, (static_cast<size_t>(State_.MaxKeys) + 1) * 2));
        SetCtrl_.resize(capacity);
        SetSlots_.resize(capacity);
        State_.SetCapacity = static_cast<int64_t>(capacity);
    } else if (State_.Want == EWant::BloomStorage) {
        BloomWords_.assign(BloomBytes_ / sizeof(uint64_t), 0);
        State_.BloomWords = static_cast<int64_t>(BloomWords_.size());
    }
    const size_t keyRoom = static_cast<size_t>(State_.KeyCount) + rows;
    if (State_.Mode != EMode::Bloom && Keys_.size() < keyRoom) {
        Keys_.resize(keyRoom);
    }
    State_.Keys = Keys_.data();
    State_.SetCtrl = SetCtrl_.data();
    State_.SetSlots = SetSlots_.data();
    State_.SortOrder = SortOrder_.data();
    State_.SortKeys = SortKeys_.data();
    State_.Bloom = BloomWords_.data();
}

TRuntimeFilterPartial TRuntimeFilterBuilder::TakePartial() && {
    Keys_.resize(static_cast<size_t>(State_.KeyCount));
    return TRuntimeFilterPartial{
        .Hashes = std::move(Keys_),
        .BloomWords = std::move(BloomWords_),
        .ExactSetOverflowed = Overflowed(),
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

    TPublishState state{.MaxKeys = static_cast<int64_t>(MaxKeys_)};
    std::vector<uint64_t> bloom;
    std::vector<uint64_t> table;
    std::vector<uint64_t> keys;
    std::vector<uint64_t> sortKeys;
    std::vector<uint32_t> sortOrder;
    // Reuse one producer's Bloom allocation instead of allocating and OR-ing
    // an initially empty global filter.
    for (auto& partial : Partials_) {
        if (!partial.BloomWords.empty()) {
            bloom.swap(partial.BloomWords);
            state.BloomSeeded = 1;
            break;
        }
    }
    if (!state.BloomSeeded) {
        keys.resize(exactHashes);
        sortKeys.resize(exactHashes);
        sortOrder.resize(SortOrderSize(exactHashes));
        table.assign(std::bit_ceil(
            std::max<size_t>(8, std::min(exactHashes, MaxKeys_) * 2)), 0);
        if (exactHashes > MaxKeys_) {
            bloom.assign(BloomBytes_ / sizeof(uint64_t), 0);
        }
    }
    std::vector<TPartialView> partials;
    partials.reserve(Partials_.size());
    for (const auto& partial : Partials_) {
        partials.push_back(TPartialView{
            .Keys = partial.Hashes.data(),
            .KeyCount = static_cast<int64_t>(partial.Hashes.size()),
            .Bloom = partial.BloomWords.data(),
            .BloomWords = static_cast<int64_t>(partial.BloomWords.size()),
        });
    }
    state.Keys = keys.data();
    state.SortOrder = sortOrder.data();
    state.SortKeys = sortKeys.data();
    state.Table = table.data();
    state.Bloom = bloom.data();
    state.BloomWords = static_cast<int64_t>(bloom.size());
    CallEntry<TPublish>(*RuntimeContext_, EEntry::Publish,
        &state, partials.data(), static_cast<int64_t>(partials.size()));

    if (state.Kind == static_cast<int64_t>(EKind::Exact)) {
        table.resize(static_cast<size_t>(state.TableSize));
        Data_.Kind = EKind::Exact;
        Data_.Words = std::move(table);
        Data_.Stats.ExactKeys = static_cast<size_t>(state.ExactKeys);
        return;
    }
    if (state.Kind != static_cast<int64_t>(EKind::Bloom)) {
        // More keys than the exact set may hold, and no Bloom storage.
        Data_.Stats.BloomDisabled = true;
        return;
    }
    Data_.Stats.BloomBytes = bloom.size() * sizeof(uint64_t);
    Data_.Stats.BloomSetBits = static_cast<size_t>(state.BloomSetBits);
    // At this density the false-positive rate approaches 50%, making the
    // extra probe unlikely to pay for itself.
    if (Data_.Stats.BloomSetBits * 10 >= bloom.size() * 64 * 9) {
        Data_.Stats.BloomDisabled = true;
        return;
    }
    Data_.Kind = EKind::Bloom;
    Data_.Words = std::move(bloom);
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
    const bool bloom = Data_.Kind == EKind::Bloom;
    const bool vectorized = bloom && rows >= VectorProbeRows;
    // The vector probe packs sparse input masks so that it also ignores rows
    // already rejected upstream. Scratch space belongs to a lane.
    if (vectorized && !selection.empty() && state.SelectedHashes.size() < hashes.size()) {
        state.SelectedHashes.resize(hashes.size());
        state.SelectedOutput.resize(hashes.size());
    }
    const TFilterData data{
        .Kind = static_cast<int64_t>(Data_.Kind),
        .Words = Data_.Words.data(),
        .WordCount = static_cast<int64_t>(Data_.Words.size()),
    };
    const auto accepted = static_cast<size_t>(CallEntry<TProbe>(*RuntimeContext_, EEntry::Probe,
        &data, hashes.data(), selection.empty() ? nullptr : selection.data(),
        output.data(), static_cast<int64_t>(hashes.size()), vectorized,
        state.SelectedHashes.data(), state.SelectedOutput.data()));
    if (bloom) {
        state.RecordBloomBatch(rows, accepted);
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
    uint32_t id, size_t producerCount, size_t bloomBytes,
    std::shared_ptr<TRuntimeContext> runtimeContext)
{
    (void)id;
    auto filter = std::make_shared<TRuntimeFilter>(
        std::move(runtimeContext), TRuntimeFilter::DefaultMaxKeys, bloomBytes);
    filter->SetProducerCount(producerCount);
    return TRuntimeFilterBinding{.Producer = filter, .Probe = std::move(filter)};
}

std::optional<size_t> ProbeRuntimeFilter(
    const TRuntimeFilterConsumer& consumer,
    TRuntimeFilterProbeState& state,
    TRowSet& rowSet,
    std::vector<uint64_t>& hashes,
    std::vector<uint8_t>& output)
{
    const size_t rows = static_cast<size_t>(rowSet.RowCount);
    const std::span<const uint8_t> selection = rowSet.Selection
        ? std::span<const uint8_t>(rowSet.Selection, rows)
        : std::span<const uint8_t>{};
    const size_t selected = selection.empty()
        ? rows
        : static_cast<size_t>(std::count_if(selection.begin(), selection.end(), [](uint8_t value) {
            return value != 0;
        }));
    if (!consumer.Probe->BeginBatch(state, selected)) {
        return std::nullopt;
    }
    hashes.resize(rows);
    if (!consumer.Hash(&rowSet, hashes.data())) {
        throw std::runtime_error("runtime filter hash kernel failed");
    }
    output.resize(rows);
    return consumer.Probe->MayContainBatch(hashes, output, state, selection);
}

} // namespace NQdb
