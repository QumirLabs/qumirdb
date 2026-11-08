#pragma once

#include <qdb/exec/plan_data.h>
#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>

namespace NQdb {

// Binds the process from its plan data and a dispatch over the kernel's slot
// (empty without computed columns).
TUnaryStreamProcess MakeProjectProcess(
    TProjectPlanData plan,
    TKernelCompiler::TProjectDispatch computeDispatch);

} // namespace NQdb
