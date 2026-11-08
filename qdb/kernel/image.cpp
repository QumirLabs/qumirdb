#include <qdb/kernel/image.h>

#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize_fused.h>
#include <qdb/modules/qumirdb.h>

#include <qumir/codegen/llvm/llvm_runner.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace NQdb {

namespace {

void WriteU64(std::string& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

void WriteString(std::string& out, const std::string& value)
{
    WriteU64(out, value.size());
    out += value;
}

class TReader {
public:
    explicit TReader(std::string_view bytes)
        : Bytes_(bytes)
    {
    }

    uint64_t ReadU64()
    {
        Need(8);
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(static_cast<uint8_t>(Bytes_[Pos_ + i])) << (8 * i);
        }
        Pos_ += 8;
        return value;
    }

    std::string ReadString()
    {
        const uint64_t size = ReadU64();
        Need(size);
        std::string value(Bytes_.substr(Pos_, size));
        Pos_ += size;
        return value;
    }

private:
    void Need(uint64_t size) const
    {
        if (size > Bytes_.size() - Pos_) {
            throw std::runtime_error("invalid kernel image: truncated");
        }
    }

    std::string_view Bytes_;
    size_t Pos_ = 0;
};

} // namespace

TKernelImage CompileKernelImage(std::span<TGeneratedKernel> kernels)
{
    // TODO: check that the slots are not bound yet.
    auto dedup = DeduplicateKernels(kernels, [](const TGeneratedKernel&) {
        return false;
    });
    auto fused = BuildFusedProgram(dedup.Unique);

    auto options = KernelRunnerOptions();
    options.NativeCode = true;
    NQumir::TLLVMRunner runner(std::move(options));
    std::string error;
    auto object = CompileKernelAstToObject(runner, fused.Program, fused.Entrypoints, &error);
    if (!object) {
        throw std::runtime_error("kernel image compilation failed: " + error);
    }

    TKernelImage image;
    image.Object = std::move(*object);
    for (const auto& [kernel, uniqueIndex] : dedup.Bindings) {
        (void)kernel;
        image.Entrypoints.push_back(fused.UniqueEntrypoints[uniqueIndex]);
    }
    return image;
}

void LoadKernelImage(const TKernelImage& image, std::span<const std::shared_ptr<TKernelSlot>> slots)
{
    // TODO: check that the image was compiled by the same binary and CPU.
    // TODO: check that the slot count matches and the slots are not bound yet;
    // FinalizeKernels treats a slot as bound by Runner || Vm, so an image slot would be recompiled.
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();

    std::vector<std::string> names;
    for (const auto& kernelNames : image.Entrypoints) {
        names.insert(names.end(), kernelNames.begin(), kernelNames.end());
    }
    NQumir::NCodeGen::TLlvmRunner linker({.EnablePerfJitEventListener = true});
    std::string error;
    auto linked = linker.LinkAndLookup({}, {image.Object}, nullptr, true, names, &error);
    if (linked.Entries.empty() && !names.empty()) {
        throw std::runtime_error("kernel image link failed: " + error);
    }

    for (size_t i = 0; i < slots.size(); ++i) {
        auto& slot = *slots[i];
        slot.Fns.clear();
        for (const auto& name : image.Entrypoints.at(i)) {
            slot.Fns.push_back(linked.Entries.at(name));
        }
        slot.JitLifetime = linked.Lifetime;
    }
}

std::string SerializeKernelImage(const TKernelImage& image)
{
    std::string out;
    WriteString(out, image.Object);
    WriteU64(out, image.Entrypoints.size());
    for (const auto& names : image.Entrypoints) {
        WriteU64(out, names.size());
        for (const auto& name : names) {
            WriteString(out, name);
        }
    }
    return out;
}

TKernelImage DeserializeKernelImage(std::string_view bytes)
{
    TReader reader(bytes);
    TKernelImage image;
    image.Object = reader.ReadString();
    for (uint64_t kernels = reader.ReadU64(); kernels > 0; --kernels) {
        auto& names = image.Entrypoints.emplace_back();
        for (uint64_t n = reader.ReadU64(); n > 0; --n) {
            names.push_back(reader.ReadString());
        }
    }
    return image;
}

} // namespace NQdb
