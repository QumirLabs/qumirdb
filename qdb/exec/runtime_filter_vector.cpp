#include <qdb/exec/runtime_filter_vector.h>

#include <qdb/kernel/compiler.h>
#include <qdb/kernel/lib.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <atomic>
#include <bit>
#include <memory>
#include <sstream>
#include <string>

namespace NQdb {
namespace {

struct TBloomVectorKernel {
    using TProbe = void (*)(const uint64_t*, const uint64_t*, uint8_t*,
        int64_t, uint64_t);

    std::unique_ptr<NQumir::TLLVMRunner> Runner;
    TProbe Probe = nullptr;

    TBloomVectorKernel() {
        NQumir::NAst::NCore::TParser parser;
        std::istringstream input(NKernel::ReadJoinKernel("bloom_vector.oz"));
        NQumir::NAst::NCore::TTokenStream tokens(input);
        auto ast = parser.Parse(tokens);
        if (!ast) {
            return;
        }
        auto options = KernelRunnerOptions();
        options.NativeCode = true;
        Runner = std::make_unique<NQumir::TLLVMRunner>(std::move(options));
        std::string error;
        auto functions = CompileKernelAst(
            *Runner, std::move(*ast), {"bloom_probe4"}, &error);
        auto found = functions.find("bloom_probe4");
        if (found != functions.end()) {
            Probe = reinterpret_cast<TProbe>(found->second);
        }
    }
};

struct TBloomVectorCache {
    enum class EStatus { Empty, Compiling, Ready, Failed };
    std::atomic<EStatus> Status{EStatus::Empty};
    std::unique_ptr<TBloomVectorKernel> Kernel;

    const TBloomVectorKernel* Get() {
        auto status = Status.load(std::memory_order_acquire);
        if (status == EStatus::Ready) {
            return Kernel.get();
        }
        if (status != EStatus::Empty || !Status.compare_exchange_strong(
                status, EStatus::Compiling, std::memory_order_acq_rel)) {
            return nullptr;
        }
        // One caller compiles; other lanes keep probing scalar while it does.
        // A failed optional optimization must not fail the query.
        try {
            Kernel = std::make_unique<TBloomVectorKernel>();
        } catch (...) {
            Status.store(EStatus::Failed, std::memory_order_release);
            return nullptr;
        }
        const bool ready = Kernel->Probe != nullptr;
        Status.store(ready ? EStatus::Ready : EStatus::Failed,
            std::memory_order_release);
        return ready ? Kernel.get() : nullptr;
    }
};

} // namespace

size_t ProbeBloomVectorBatch(
    std::span<const uint64_t> words,
    std::span<const uint64_t> hashes,
    std::span<uint8_t> output)
{
    constexpr size_t WordsPerBlock = 8;
    if (hashes.size() != output.size()
        || words.size() < WordsPerBlock
        || words.size() % WordsPerBlock != 0
        || !std::has_single_bit(words.size() / WordsPerBlock)
        || hashes.size() < 8192)
    {
        return 0;
    }
    static TBloomVectorCache cache;
    const auto* kernel = cache.Get();
    if (!kernel) {
        return 0;
    }
    const size_t count = hashes.size() & ~size_t{3};
    kernel->Probe(words.data(), hashes.data(), output.data(),
        static_cast<int64_t>(count),
        static_cast<uint64_t>(words.size() / WordsPerBlock - 1));
    return count;
}

} // namespace NQdb
