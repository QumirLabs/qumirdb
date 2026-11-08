#pragma once

#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace NQdb {

// What a filter process needs besides its kernel, so it can be rebuilt later.
struct TFilterPlanData {
    // Index of the filter kernel in the sink (0 without a sink).
    uint32_t Kernel = 0;
    // Input columns kept in output order; nullopt keeps all of them.
    std::optional<std::vector<int32_t>> KeptInputColumns;
};

TUnaryStreamProcess MakeFilterProcess(
    TKernelCompiler::TFilterDispatch dispatch);

// Binds the process from its plan data and a dispatch over the kernel's slot.
TUnaryStreamProcess MakeFilterProcess(
    const TFilterPlanData& plan,
    TKernelCompiler::TFilterDispatch dispatch);

// Filter that also drops input columns absent from the output schema: applies
// the predicate (which still sees every input column) then emits only the kept
// columns (zero-copy). keptIndices are input column indices, in output order.
TUnaryStreamProcess MakeFilterSelectProcess(
    TKernelCompiler::TFilterDispatch dispatch,
    std::vector<int32_t> keptIndices);

} // namespace NQdb
