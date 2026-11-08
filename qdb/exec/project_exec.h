#pragma once

#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace NQdb {

// One output column: either a zero-copy reference to an input column (ident
// projection) or a computed column filled by the project kernel.
struct TProjectColumn {
    bool Computed = false;
    // Ident: index into the input batch's columns.
    // Computed: index into the flat computed-buffer list.
    int32_t Index = 0;
};

// What a project process needs besides its kernel, so it can be rebuilt later.
struct TProjectPlanData {
    // Index of the project kernel in the sink; none without computed columns.
    std::optional<uint32_t> Kernel;
    std::vector<TProjectColumn> Columns;
    std::vector<size_t> ComputedWidths;
    std::vector<bool> ComputedIsString;
    std::vector<bool> ComputedIsNullable;
};

// Binds the process from its plan data and a dispatch over the kernel's slot
// (empty without computed columns).
TUnaryStreamProcess MakeProjectProcess(
    TProjectPlanData plan,
    TKernelCompiler::TProjectDispatch computeDispatch);

} // namespace NQdb
