#pragma once

#include <qdb/exec/plan.h>
#include <qdb/scheduler/lowering/plan_lowerer.h>

#include <expected>
#include <string>

namespace NQdb {

std::expected<TExecPlan, std::string> BuildExecPlan(
    const NScheduler::TLoweredPlan& lowered);

} // namespace NQdb
