#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/runtime_filter_vector.h>

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace NQdb {

namespace {

// Reserve zero for empty slots.
uint64_t Storable(uint64_t hash) {
    return hash == 0 ? 1 : hash;
}

// One cache line per block, with one bit in each of its eight words. The
// published filter and every producer use identical positions, so fragments
// merge with a bitwise OR.
constexpr size_t BloomWordsPerBlock = 8;
constexpr size_t BloomBytesPerBlock = BloomWordsPerBlock * sizeof(uint64_t);

size_t BloomWordCount(size_t bytes) {
    return (bytes / BloomBytesPerBlock) * BloomWordsPerBlock;
}

uint64_t Mix(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

size_t BloomBlock(const std::vector<uint64_t>& words, uint64_t hash) {
    return static_cast<size_t>(Mix(hash) % (words.size() / BloomWordsPerBlock))
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

} // namespace

void IRuntimeFilterProbe::MayContainBatch(
    std::span<const uint64_t> hashes,
    std::span<uint8_t> output) const
{
    if (hashes.size() != output.size()) {
        throw std::invalid_argument("runtime filter batch size mismatch");
    }
    for (size_t i = 0; i < hashes.size(); ++i) {
        output[i] = MayContain(hashes[i]) ? 0xff : 0;
    }
}

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
        if (BloomBytes_ >= BloomBytesPerBlock) {
            BloomWords_.assign(BloomWordCount(BloomBytes_), 0);
            for (uint64_t hash : Hashes_) {
                BloomAdd(BloomWords_, hash);
            }
        }
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
        .BloomWords = std::move(BloomWords_),
        .Bounds = Bounds_
            ? std::optional(std::pair{Bounds_->Min, Bounds_->Max})
            : std::nullopt,
        .BoundsUsable = boundsUsable,
        .ExactSetOverflowed = Overflowed_,
        .BuildRows = BuildRows_,
    };
}

void TRuntimeFilter::Merge(TRuntimeFilterBuilder&& builder) {
    Merge(std::move(builder).TakePartial());
}

void TRuntimeFilter::Merge(TRuntimeFilterPartial&& partial) {
    std::lock_guard guard(Mutex_);
    BuildRows_ += partial.BuildRows;
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
    if (ExactSetDisabled() && BloomWords_.empty()) {
        return; // Bloom saturated or disabled: pass everything.
    }
    if (partial.ExactSetOverflowed && partial.BloomWords.empty()) {
        ExactSetDisabled_.store(true, std::memory_order_relaxed);
        BloomWords_.clear();
        Pending_.clear();
        Pending_.shrink_to_fit();
        return;
    }
    if (!partial.BloomWords.empty()) {
        if (BloomWords_.empty()) {
            PromoteToBloom();
        }
        if (BloomWords_.size() != partial.BloomWords.size()) {
            // A transport/configuration mismatch must never reject a key.
            BloomWords_.clear();
            ExactSetDisabled_.store(true, std::memory_order_relaxed);
            return;
        }
        for (size_t i = 0; i < BloomWords_.size(); ++i) {
            BloomWords_[i] |= partial.BloomWords[i];
        }
    }
    if (!BloomWords_.empty()) {
        for (uint64_t hash : partial.Hashes) {
            BloomAdd(BloomWords_, hash);
        }
        return;
    }
    Pending_.insert(Pending_.end(),
        partial.Hashes.begin(), partial.Hashes.end());
    if (Pending_.size() >= MaxKeys_ * 2) {
        std::ranges::sort(Pending_);
        const auto duplicates = std::ranges::unique(Pending_);
        Pending_.erase(duplicates.begin(), duplicates.end());
        if (Pending_.size() > MaxKeys_) {
            PromoteToBloom();
        }
    }
}

void TRuntimeFilter::PromoteToBloom() {
    ExactSetDisabled_.store(true, std::memory_order_relaxed);
    if (BloomBytes_ >= BloomBytesPerBlock) {
        BloomWords_.assign(BloomWordCount(BloomBytes_), 0);
        BloomAllocatedBytes_ = BloomWords_.size() * sizeof(uint64_t);
        for (uint64_t hash : Pending_) {
            BloomAdd(BloomWords_, hash);
        }
    }
    Pending_.clear();
    Pending_.shrink_to_fit();
}

void TRuntimeFilter::SetProducerCount(size_t producers) {
    PendingProducers_.store(producers, std::memory_order_relaxed);
}

void TRuntimeFilter::StartExecutionClock() {
    ExecutionStart_ = std::chrono::steady_clock::now();
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
                PromoteToBloom();
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
        if (!BloomWords_.empty()) {
            size_t setBits = 0;
            for (uint64_t word : BloomWords_) {
                setBits += std::popcount(word);
            }
            BloomSetBits_ = setBits;
            // At this density the false-positive rate approaches 50%; the
            // extra probe is unlikely to pay for itself.
            if (setBits * 10 >= BloomWords_.size() * 64 * 9) {
                BloomDisabled_ = true;
                BloomWords_.clear();
            }
        }
    }
    if (ExecutionStart_ != std::chrono::steady_clock::time_point{}) {
        ReadyLatencyMs_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ExecutionStart_).count();
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
    if (!Ready()) {
        return true;
    }
    if (ExactSetDisabled()) {
        return BloomWords_.empty() || BloomMayContain(BloomWords_, hash);
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

void TRuntimeFilter::MayContainBatch(
    std::span<const uint64_t> hashes,
    std::span<uint8_t> output) const
{
    if (hashes.size() != output.size()) {
        throw std::invalid_argument("runtime filter batch size mismatch");
    }
    if (hashes.empty()) {
        return;
    }
    ProbeRows_.fetch_add(hashes.size(), std::memory_order_relaxed);
    if (!Ready() || (ExactSetDisabled() && BloomWords_.empty())) {
        std::fill(output.begin(), output.end(), 0xff);
        SkippedProbeRows_.fetch_add(hashes.size(), std::memory_order_relaxed);
        return;
    }
    if (ExactSetDisabled()) {
        if (SkipWeakBloomBatch()) {
            std::fill(output.begin(), output.end(), 0xff);
            SkippedProbeRows_.fetch_add(hashes.size(), std::memory_order_relaxed);
            PausedProbeRows_.fetch_add(hashes.size(), std::memory_order_relaxed);
            return;
        }
        // The vector kernel is JIT-compiled once per process. Small scans are
        // cheaper to finish scalar than to pay for that compilation.
        constexpr size_t VectorCompileThreshold = 1'000'000;
        const size_t seen = BloomProbeRows_.fetch_add(
            hashes.size(), std::memory_order_relaxed);
        const size_t vectorized =
            (seen >= VectorCompileThreshold
                || hashes.size() >= VectorCompileThreshold - seen)
            ? ProbeBloomVectorBatch(BloomWords_, hashes, output)
            : 0;
        for (size_t i = vectorized; i < hashes.size(); ++i) {
            output[i] = BloomMayContain(BloomWords_, hashes[i]) ? 0xff : 0;
        }
        const size_t accepted = static_cast<size_t>(std::count_if(
            output.begin(), output.end(), [](uint8_t value) { return value != 0; }));
        RejectedRows_.fetch_add(hashes.size() - accepted,
            std::memory_order_relaxed);
        RecordBloomBatch(hashes.size(), accepted);
        return;
    }
    size_t rejected = 0;
    for (size_t i = 0; i < hashes.size(); ++i) {
        const uint64_t stored = Storable(hashes[i]);
        uint64_t slot = hashes[i] & Mask_;
        while (Table_[slot] != 0 && Table_[slot] != stored) {
            slot = (slot + 1) & Mask_;
        }
        output[i] = Table_[slot] == stored ? 0xff : 0;
        rejected += output[i] == 0;
    }
    RejectedRows_.fetch_add(rejected, std::memory_order_relaxed);
}

bool TRuntimeFilter::SkipWeakBloomBatch() const {
    std::lock_guard guard(ProbePolicyMutex_);
    if (!PauseBatches_) {
        return false;
    }
    --PauseBatches_;
    return true;
}

void TRuntimeFilter::RecordBloomBatch(size_t rows, size_t accepted) const {
    std::lock_guard guard(ProbePolicyMutex_);
    SampleRows_ += rows;
    SampleAccepted_ += accepted;
    ++SampleBatches_;
    // Require both several batches and enough rows: tiny task batches should
    // not make the global probe policy oscillate on sparse samples.
    if (SampleBatches_ < 6 || SampleRows_ < 32768) {
        return;
    }
    if (SampleAccepted_ * 2 >= SampleRows_) {
        PauseMultiplier_ = std::min<size_t>(PauseMultiplier_ + 1, 16);
        PauseBatches_ = PauseMultiplier_ * 4;
    } else {
        PauseMultiplier_ = 0;
    }
    SampleRows_ = 0;
    SampleAccepted_ = 0;
    SampleBatches_ = 0;
}

TRuntimeFilterStats TRuntimeFilter::StatsSnapshot() const {
    const bool ready = Ready();
    return TRuntimeFilterStats{
        .BuildRows = BuildRows_,
        .ProbeRows = ProbeRows_.load(std::memory_order_relaxed),
        .RejectedRows = RejectedRows_.load(std::memory_order_relaxed),
        .SkippedProbeRows = SkippedProbeRows_.load(std::memory_order_relaxed),
        .PausedProbeRows = PausedProbeRows_.load(std::memory_order_relaxed),
        .BloomBytes = BloomAllocatedBytes_,
        .BloomSetBits = BloomSetBits_,
        .ExactKeys = KeyCount_,
        .ReadyLatencyMs = ReadyLatencyMs_,
        .Ready = ready,
        .BloomDisabled = BloomDisabled_,
    };
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
    return CreateSized(id, producerCount, TRuntimeFilter::DefaultBloomBytes);
}

TRuntimeFilterBinding TLocalRuntimeFilterBindingFactory::CreateSized(
    uint32_t id, size_t producerCount, size_t bloomBytes)
{
    (void)id;
    auto filter = std::make_shared<TRuntimeFilter>(
        TRuntimeFilter::DefaultMaxKeys, bloomBytes);
    filter->SetProducerCount(producerCount);
    return TRuntimeFilterBinding{
        .Producer = filter,
        .Probe = std::move(filter),
    };
}

} // namespace NQdb
