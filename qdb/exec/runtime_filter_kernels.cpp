#include <qdb/exec/runtime_filter_kernels.h>
#include <qdb/kernel/generated.h>

#include <algorithm>
#include <bit>

namespace NQdb {
namespace {

// Parallel to TRuntimeFilterKernels::Entrypoints().
enum EEntry : size_t {
    BloomProbeScalar,
    BloomProbeVector,
    BloomAdd,
    BloomOr,
    BloomPopcount,
    ExactBuild,
    ExactProbe,
    KeysAppend,
    KeysSortUnique,
    SetFill,
    KeysAppendDistinct,
};

constexpr size_t BloomWordsPerBlock = 8;

using TProbe = int64_t (*)(const uint64_t*, uint64_t, const uint64_t*,
    const uint8_t*, uint8_t*, int64_t);

template <typename T>
T Entry(const TKernelSlot& slot, EEntry entry) {
    return reinterpret_cast<T>(slot.Fns[entry]);
}

uint64_t BloomBlockMask(std::span<const uint64_t> words) {
    return words.size() / BloomWordsPerBlock - 1;
}

const uint8_t* SelectionData(std::span<const uint8_t> selection) {
    return selection.empty() ? nullptr : selection.data();
}

// Keeps the set at most half full, in whole swiss groups.
size_t SetCapacity(size_t maxKeys) {
    return std::bit_ceil(std::max<size_t>(16, (maxKeys + 1) * 2));
}

} // namespace

const std::vector<std::string>& TRuntimeFilterKernels::Entrypoints() {
    static const std::vector<std::string> names = {
        "rf_bloom_probe_scalar",
        "rf_bloom_probe_vec",
        "rf_bloom_add",
        "rf_bloom_or",
        "rf_bloom_popcount",
        "rf_exact_build",
        "rf_exact_probe",
        "rf_keys_append",
        "rf_keys_sort_unique",
        "rf_set_fill",
        "rf_keys_append_distinct",
    };
    return names;
}

size_t TRuntimeFilterKernels::BloomProbeScalar(
    std::span<const uint64_t> words,
    std::span<const uint64_t> hashes,
    std::span<const uint8_t> selection,
    std::span<uint8_t> output) const
{
    return static_cast<size_t>(Entry<TProbe>(*Slot_, EEntry::BloomProbeScalar)(
        words.data(), BloomBlockMask(words), hashes.data(), SelectionData(selection),
        output.data(), static_cast<int64_t>(hashes.size())));
}

size_t TRuntimeFilterKernels::BloomProbeVector(
    std::span<const uint64_t> words,
    std::span<const uint64_t> hashes,
    std::span<const uint8_t> selection,
    std::span<uint8_t> output,
    std::vector<uint64_t>& selectedHashes,
    std::vector<uint8_t>& selectedOutput) const
{
    using TProbeVector = int64_t (*)(const uint64_t*, uint64_t, const uint64_t*,
        const uint8_t*, uint8_t*, int64_t, uint64_t*, uint8_t*);
    if (!selection.empty() && selectedHashes.size() < hashes.size()) {
        selectedHashes.resize(hashes.size());
        selectedOutput.resize(hashes.size());
    }
    return static_cast<size_t>(Entry<TProbeVector>(*Slot_, EEntry::BloomProbeVector)(
        words.data(), BloomBlockMask(words), hashes.data(), SelectionData(selection),
        output.data(), static_cast<int64_t>(hashes.size()),
        selectedHashes.data(), selectedOutput.data()));
}

void TRuntimeFilterKernels::BloomAdd(
    std::span<uint64_t> words,
    std::span<const uint64_t> hashes,
    std::span<const uint8_t> selection) const
{
    if (words.empty()) {
        return;
    }
    using TAdd = void (*)(uint64_t*, uint64_t, const uint64_t*, const uint8_t*, int64_t);
    Entry<TAdd>(*Slot_, EEntry::BloomAdd)(words.data(), BloomBlockMask(words),
        hashes.data(), SelectionData(selection), static_cast<int64_t>(hashes.size()));
}

void TRuntimeFilterKernels::BloomOr(
    std::span<uint64_t> target, std::span<const uint64_t> source) const
{
    using TOr = void (*)(uint64_t*, const uint64_t*, int64_t);
    Entry<TOr>(*Slot_, EEntry::BloomOr)(target.data(), source.data(),
        static_cast<int64_t>(source.size()));
}

size_t TRuntimeFilterKernels::BloomSetBits(std::span<const uint64_t> words) const {
    using TPopcount = int64_t (*)(const uint64_t*, int64_t);
    return static_cast<size_t>(Entry<TPopcount>(*Slot_, EEntry::BloomPopcount)(
        words.data(), static_cast<int64_t>(words.size())));
}

void TRuntimeFilterKernels::ExactBuild(
    std::span<uint64_t> table, std::span<const uint64_t> keys) const
{
    using TBuild = void (*)(uint64_t*, uint64_t, const uint64_t*, int64_t);
    Entry<TBuild>(*Slot_, EEntry::ExactBuild)(table.data(), table.size() - 1,
        keys.data(), static_cast<int64_t>(keys.size()));
}

size_t TRuntimeFilterKernels::ExactProbe(
    std::span<const uint64_t> table,
    std::span<const uint64_t> hashes,
    std::span<const uint8_t> selection,
    std::span<uint8_t> output) const
{
    return static_cast<size_t>(Entry<TProbe>(*Slot_, EEntry::ExactProbe)(
        table.data(), table.size() - 1, hashes.data(), SelectionData(selection),
        output.data(), static_cast<int64_t>(hashes.size())));
}

size_t TRuntimeFilterKernels::AppendKeys(
    std::vector<uint64_t>& keys,
    size_t capacity,
    std::span<const uint64_t> hashes,
    std::span<const uint8_t> selection) const
{
    using TAppend = int64_t (*)(uint64_t*, int64_t*, int64_t, const uint64_t*,
        const uint8_t*, int64_t);
    auto size = static_cast<int64_t>(keys.size());
    capacity = std::min(capacity, keys.size() + hashes.size());
    keys.resize(std::max(capacity, keys.size()));
    const int64_t consumed = Entry<TAppend>(*Slot_, EEntry::KeysAppend)(keys.data(), &size,
        static_cast<int64_t>(capacity), hashes.data(), SelectionData(selection),
        static_cast<int64_t>(hashes.size()));
    keys.resize(static_cast<size_t>(size));
    return static_cast<size_t>(consumed);
}

void TRuntimeFilterKernels::SortUnique(std::vector<uint64_t>& keys) const {
    using TSortUnique = int64_t (*)(uint64_t*, int64_t, uint32_t*, uint32_t*,
        uint32_t*, uint64_t*);
    std::vector<uint32_t> order(keys.size() * 2 + 256);
    std::vector<uint64_t> sorted(keys.size());
    const int64_t distinct = Entry<TSortUnique>(*Slot_, EEntry::KeysSortUnique)(
        keys.data(), static_cast<int64_t>(keys.size()), order.data(),
        order.data() + keys.size(), order.data() + keys.size() * 2, sorted.data());
    keys.resize(static_cast<size_t>(distinct));
}

void TRuntimeFilterKernels::FillSet(
    TRuntimeFilterKeySet& set,
    size_t maxKeys,
    std::span<const uint64_t> keys) const
{
    using TFill = void (*)(uint8_t*, uint64_t*, int64_t, const uint64_t*, int64_t);
    const size_t capacity = SetCapacity(maxKeys);
    set.Ctrl.resize(capacity);
    set.Slots.resize(capacity);
    Entry<TFill>(*Slot_, EEntry::SetFill)(set.Ctrl.data(), set.Slots.data(),
        static_cast<int64_t>(capacity), keys.data(), static_cast<int64_t>(keys.size()));
}

size_t TRuntimeFilterKernels::AppendDistinctKeys(
    TRuntimeFilterKeySet& set,
    std::vector<uint64_t>& keys,
    size_t maxKeys,
    std::span<const uint64_t> hashes,
    std::span<const uint8_t> selection) const
{
    using TAppend = int64_t (*)(uint8_t*, uint64_t*, int64_t, uint64_t*, int64_t*,
        int64_t, const uint64_t*, const uint8_t*, int64_t);
    auto size = static_cast<int64_t>(keys.size());
    keys.resize(std::max(keys.size(), std::min(maxKeys + 1, keys.size() + hashes.size())));
    const int64_t consumed = Entry<TAppend>(*Slot_, EEntry::KeysAppendDistinct)(
        set.Ctrl.data(), set.Slots.data(), static_cast<int64_t>(set.Ctrl.size()),
        keys.data(), &size, static_cast<int64_t>(maxKeys), hashes.data(),
        SelectionData(selection), static_cast<int64_t>(hashes.size()));
    keys.resize(static_cast<size_t>(size));
    return static_cast<size_t>(consumed);
}

} // namespace NQdb
