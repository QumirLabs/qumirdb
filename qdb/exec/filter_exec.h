#pragma once

#include <qdb/exec/plan_data.h>
#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <vector>

namespace NQdb {

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
