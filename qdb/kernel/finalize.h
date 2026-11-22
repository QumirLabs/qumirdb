#pragma once

#include <qdb/kernel/backend.h>
#include <qdb/kernel/generated.h>

#include <qumir/parser/type_factory.h>

#include <iosfwd>
#include <memory>
#include <span>
#include <string>

namespace NQdb {

class TExternalCatalogSnapshot;

// Compile the kernels' ASTs, built in `types`, as one program for `backend` and fill
// their slots: native entry points from the LLVM JIT, or prepared Qumir VM bytecode.
// Each bound slot keeps `types`.
// Already-bound slots are skipped; a slot bound for the other backend is a
// logic error. A non-empty cacheDir persists the compiled LLVM dependencies
// there. Throws on compilation failure.
void FinalizeKernels(
    std::span<TGeneratedKernel> kernels,
    std::shared_ptr<NQumir::NAst::TTypeFactory> types,
    EKernelBackend backend,
    std::ostream* diagnostics = nullptr,
    std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog = nullptr,
    const std::string& cacheDir = {});

} // namespace NQdb
