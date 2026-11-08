#pragma once

#include <qdb/kernel/generated.h>

#include <qumir/parser/ast.h>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NQdb {

// Dedup key for a whole kernel: identical AST + entrypoints compile once.
std::string MakeKernelDedupKey(const TGeneratedKernel& kernel);

struct TKernelDedup {
    std::vector<TGeneratedKernel*> Unique;
    // Every kept kernel with the index of its unique kernel.
    std::vector<std::pair<TGeneratedKernel*, size_t>> Bindings;
};

// Groups kernels by MakeKernelDedupKey; kernels for which `skip` is true are left out.
TKernelDedup DeduplicateKernels(
    std::span<TGeneratedKernel> kernels,
    const std::function<bool(const TGeneratedKernel&)>& skip);

struct TFusedProgram {
    NQumir::NAst::TExprPtr Program;
    std::vector<std::string> Entrypoints;                    // deduped, for the compiler
    std::vector<std::vector<std::string>> UniqueEntrypoints; // per input kernel, fused names
    // Per input kernel: renamed function names (original -> fused). Only entries
    // that changed; callers apply `renames[name]` falling back to `name`.
    std::vector<std::unordered_map<std::string, std::string>> UniqueRenames;
    size_t TypeDeclCount = 0;
    size_t FunctionDeclCount = 0;
};

// Merges kernels into one program: identical decls shared, conflicting
// query-private decls prefixed __qdb_k<i>_ (rename propagated to callers).
// Reusable helpers with identical bodies keep their canonical names.
TFusedProgram BuildFusedProgram(std::span<TGeneratedKernel* const> uniqueKernels);

} // namespace NQdb
