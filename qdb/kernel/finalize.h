#pragma once

#include <qdb/kernel/backend.h>
#include <qdb/kernel/generated.h>

#include <iosfwd>
#include <memory>
#include <span>
#include <string>

namespace NQdb {

class TExternalCatalogSnapshot;

// Compile the kernels' ASTs as one program for `backend` and fill their slots:
// native entry points from the LLVM JIT, or prepared Qumir VM bytecode.
// Already-bound slots are skipped; a slot bound for the other backend is a
// logic error. A non-empty cacheDir persists the compiled LLVM dependencies
// there. Throws on compilation failure.
void FinalizeKernels(
    std::span<TGeneratedKernel> kernels,
    EKernelBackend backend,
    std::ostream* diagnostics = nullptr,
    std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog = nullptr,
    const std::string& cacheDir = {});

} // namespace NQdb
