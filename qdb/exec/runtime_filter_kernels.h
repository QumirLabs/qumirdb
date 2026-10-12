#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace NQdb {

struct TKernelSlot;

// Storage for the kernel's deduplicating hash set. Only the kernel reads it.
struct TRuntimeFilterKeySet {
    std::vector<uint8_t> Ctrl;
    std::vector<uint64_t> Slots;
};

// Typed calls into the runtime-filter kernel (qdb/kernel/join/runtime_filter.oz),
// which implements every operation on filter contents. Buffers stay with the
// caller. Requires a finalized slot. An empty selection selects every row.
class TRuntimeFilterKernels {
public:
    // In TKernelSlot::Fns order.
    static const std::vector<std::string>& Entrypoints();

    explicit TRuntimeFilterKernels(const TKernelSlot& slot)
        : Slot_(&slot)
    {}

    // Bloom words hold a power-of-two number of 64-byte blocks. Probes write
    // nonzero for a kept row and 0 otherwise, and return the number kept.
    size_t BloomProbeScalar(
        std::span<const uint64_t> words,
        std::span<const uint64_t> hashes,
        std::span<const uint8_t> selection,
        std::span<uint8_t> output) const;
    // The scratch vectors hold the packed selection; they only grow.
    size_t BloomProbeVector(
        std::span<const uint64_t> words,
        std::span<const uint64_t> hashes,
        std::span<const uint8_t> selection,
        std::span<uint8_t> output,
        std::vector<uint64_t>& selectedHashes,
        std::vector<uint8_t>& selectedOutput) const;
    // No-op when the filter has no Bloom storage.
    void BloomAdd(
        std::span<uint64_t> words,
        std::span<const uint64_t> hashes,
        std::span<const uint8_t> selection = {}) const;
    void BloomOr(std::span<uint64_t> target, std::span<const uint64_t> source) const;
    size_t BloomSetBits(std::span<const uint64_t> words) const;

    // The table has a power-of-two size and starts zeroed.
    void ExactBuild(std::span<uint64_t> table, std::span<const uint64_t> keys) const;
    size_t ExactProbe(
        std::span<const uint64_t> table,
        std::span<const uint64_t> hashes,
        std::span<const uint8_t> selection,
        std::span<uint8_t> output) const;

    // Appends selected hashes until keys holds capacity entries. Returns the
    // number of input rows consumed.
    size_t AppendKeys(
        std::vector<uint64_t>& keys,
        size_t capacity,
        std::span<const uint64_t> hashes,
        std::span<const uint8_t> selection) const;
    void SortUnique(std::vector<uint64_t>& keys) const;

    // Resets the set to the given distinct keys, sized for maxKeys + 1 entries.
    void FillSet(
        TRuntimeFilterKeySet& set,
        size_t maxKeys,
        std::span<const uint64_t> keys) const;
    // Appends selected hashes missing from the set and stops right after keys
    // exceeds maxKeys. Returns the number of input rows consumed.
    size_t AppendDistinctKeys(
        TRuntimeFilterKeySet& set,
        std::vector<uint64_t>& keys,
        size_t maxKeys,
        std::span<const uint64_t> hashes,
        std::span<const uint8_t> selection) const;

private:
    const TKernelSlot* Slot_;
};

} // namespace NQdb
