#include <qdb/exec/runtime_filter_vector.h>

#include <qdb/kernel/compiler.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <bit>
#include <memory>
#include <sstream>
#include <string>

namespace NQdb {
namespace {

constexpr const char* BloomVectorSource = R"(
(block
  (fun bloom_mix4 ((var value <vec u64 4>)) -> <vec u64 4>
    (block
      (= value (^ value (>> value 30)))
      (= value (* value (cast -4658895280553007687 u64)))
      (= value (^ value (>> value 27)))
      (= value (* value (cast -7723592293110705685 u64)))
      (return (^ value (>> value 31)))))
  (fun bloom_probe4
       ((var words <ptr u64>) (var hashes <ptr u64>)
        (var output <ptr u8>) (var count i64) (var block_mask u64))
    (block
      (var row = 0)
      (while (< row count)
        (block
          (var hash = (vec (index hashes row)
                         (index hashes (+ row 1))
                         (index hashes (+ row 2))
                         (index hashes (+ row 3))))
          (var block = (* (& (call bloom_mix4 hash) block_mask) (cast 8 u64)))
          (var bits = (call bloom_mix4
                         (^ hash (cast -7046029254386353131 u64))))
          (var alive = (vec #t #t #t #t))
          (var word = 0)
          (while (&& (< word 8) (call builtin::any alive))
            (block
              (var found = (call builtin::gather words (+ block (cast word u64))))
              (var bit = (<< (cast 1 u64)
                           (& (>> bits (* word 8)) (cast 63 u64))))
              (= alive (& alive (!= (& found bit) (cast 0 u64))))
              (= word (+ word 1))))
          (= output[row] (cast (index alive 0) u8))
          (= output[(+ row 1)] (cast (index alive 1) u8))
          (= output[(+ row 2)] (cast (index alive 2) u8))
          (= output[(+ row 3)] (cast (index alive 3) u8))
          (= row (+ row 4)))))))
)";

struct TBloomVectorKernel {
    using TProbe = void (*)(const uint64_t*, const uint64_t*, uint8_t*,
        int64_t, uint64_t);

    NQumir::NCodeGen::TLLVMInitializer Llvm;
    std::unique_ptr<NQumir::TLLVMRunner> Runner;
    TProbe Probe = nullptr;

    TBloomVectorKernel() {
        NQumir::NAst::NCore::TParser parser;
        std::istringstream input(BloomVectorSource);
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
