#include <qdb/exec/runtime_filter_vector.h>

#include <qdb/kernel/compiler.h>
#include <qdb/kernel/lib.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <bit>
#include <memory>
#include <sstream>
#include <string>

namespace NQdb {
namespace {

struct TBloomVectorKernel {
    using TProbe = void (*)(const uint64_t*, const uint64_t*, uint8_t*,
        int64_t, uint64_t);

    NQumir::NCodeGen::TLLVMInitializer Llvm;
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
    static const TBloomVectorKernel kernel;
    if (!kernel.Probe) {
        return 0;
    }
    const size_t count = hashes.size() & ~size_t{3};
    kernel.Probe(words.data(), hashes.data(), output.data(),
        static_cast<int64_t>(count),
        static_cast<uint64_t>(words.size() / WordsPerBlock - 1));
    return count;
}

} // namespace NQdb
