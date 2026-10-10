#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace NQdb {

// Returns the number of hashes handled by the qumir vector kernel. The caller
// probes any remainder with the scalar path.
size_t ProbeBloomVectorBatch(
    std::span<const uint64_t> words,
    std::span<const uint64_t> hashes,
    std::span<uint8_t> output);

} // namespace NQdb
