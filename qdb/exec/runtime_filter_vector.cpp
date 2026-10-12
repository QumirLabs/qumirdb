#include <qdb/exec/runtime_filter_vector.h>
#include <qdb/kernel/generated.h>

#include <bit>

namespace NQdb {

size_t ProbeBloomVectorBatch(
    const TKernelSlot& kernel,
    std::span<const uint64_t> words,
    std::span<const uint64_t> hashes,
    std::span<uint8_t> output)
{
    constexpr size_t wordsPerBlock = 8;
    if (hashes.size() != output.size()
        || words.size() < wordsPerBlock
        || words.size() % wordsPerBlock != 0
        || !std::has_single_bit(words.size() / wordsPerBlock)
        || hashes.size() < 8192)
    {
        return 0;
    }
    const size_t count = hashes.size() & ~size_t{3};
    using TProbe = void (*)(const uint64_t*, const uint64_t*, uint8_t*,
        int64_t, uint64_t);
    const auto probe = reinterpret_cast<TProbe>(kernel.Fns[0]);
    probe(words.data(), hashes.data(), output.data(),
        count, words.size() / wordsPerBlock - 1);
    return count;
}

} // namespace NQdb
