#pragma once

#include <qdb/kernel/generated.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace NQdb {

// Compiled LLVM code of kernels that another process loads without compiling.
// Object is one relocatable PIC object; runtime helpers stay undefined symbols
// that the loader binds by name.
struct TKernelImage {
    std::string Object;
    // Cached library objects that Object calls; empty without a cache dir.
    std::vector<std::string> Dependencies;
    // Per kernel: the linked entry names, parallel to TGeneratedKernel::Entrypoints.
    std::vector<std::vector<std::string>> Entrypoints;
};

// Deduplicates and fuses `kernels` as FinalizeKernels does and compiles them once; binds no slot.
// A non-empty cacheDir persists the library objects there (created if missing) and the image
// carries them in Dependencies. Fusion rewrites the ASTs, so the kernels cannot be finalized afterwards.
TKernelImage CompileKernelImage(std::span<TGeneratedKernel> kernels, const std::string& cacheDir = {});

// Links the image into this process and binds slots[i] to kernel i.
void LoadKernelImage(const TKernelImage& image, std::span<const std::shared_ptr<TKernelSlot>> slots);

std::string SerializeKernelImage(const TKernelImage& image);
// Throws std::runtime_error on truncated input.
TKernelImage DeserializeKernelImage(std::string_view bytes);

} // namespace NQdb
