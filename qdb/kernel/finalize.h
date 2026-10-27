#pragma once

#include <qdb/kernel/generated.h>

#include <iosfwd>
#include <memory>
#include <span>
#include <string>

namespace NQdb {

class TExternalCatalogSnapshot;

// Compile each kernel's AST with the native JIT and fill its slot (Fns per
// entrypoint + owning runner). Idempotent - already-bound slots are skipped.
// A non-empty cacheDir persists the compiled dependencies there.
// Throws on compilation failure.
void JitFinalizeKernels(
    std::span<TGeneratedKernel> kernels,
    std::ostream* diagnostics = nullptr,
    std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog = nullptr,
    const std::string& cacheDir = {});

} // namespace NQdb
