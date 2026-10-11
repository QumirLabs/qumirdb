#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace NQdb {

struct TKernelSlot;

// Returns the number of hashes handled by the qumir vector kernel. The caller
// probes any remainder with the scalar path. Requires a finalized Bloom slot.
size_t ProbeBloomVectorBatch(
    const TKernelSlot& kernel,
    std::span<const uint64_t> words,
    std::span<const uint64_t> hashes,
    std::span<uint8_t> output);

} // namespace NQdb
