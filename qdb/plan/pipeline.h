#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/late_materialization.h>
#include <qdb/plan/passes/typing.h>

#include <iosfwd>
#include <memory>

namespace NQdb {

class TExternalCatalogSnapshot;

struct TPlanPassDiagnostics {
    TJoinReorderDiagnostics JoinReorder;
    TLateMaterializationDiagnostics LateMaterialization;
};

struct TPlanPassOptions {
    bool EnableCbo = true;
    bool EnableRuntimeFilters = true;
    // Measurement switch; see AttachRuntimeFilters.
    bool ForceRuntimeFilters = false;
    TLateMaterializationSettings LateMaterialization;
    TPlanPassDiagnostics* Diagnostics = nullptr;
    NKernel::TAnnotationContext Annotation;
    // When set, the plan is dumped here after each pass (debugging).
    std::ostream* DumpPasses = nullptr;
};

void ApplyPlanPasses(TOperatorPtr& plan, TPlanPassOptions options = {});

} // namespace NQdb
