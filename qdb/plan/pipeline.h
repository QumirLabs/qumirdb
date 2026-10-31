#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/late_materialization.h>
#include <qdb/plan/passes/typing.h>

#include <iosfwd>
#include <memory>
#include <optional>
#include <string_view>

namespace NQdb {

class TExternalCatalogSnapshot;

struct TPlanPassDiagnostics {
    TJoinReorderDiagnostics JoinReorder;
    TLateMaterializationDiagnostics LateMaterialization;
};

// How SQL subqueries are decorrelated. Legacy: BuildPlan rewrites them by
// patterns while it builds. New: BuildCanonicalPlan attaches them with
// dependent joins, which an unnesting pass is to remove; until it exists such
// a plan is only explained. The planner reads the choice; the passes ignore it.
enum class EDecorrelation {
    Legacy = 0,
    New = 1,
};

// The names the config accepts.
inline constexpr std::string_view DecorrelationNames = "legacy, new";

inline std::optional<EDecorrelation> ParseDecorrelation(std::string_view name) {
    if (name == "legacy") return EDecorrelation::Legacy;
    if (name == "new") return EDecorrelation::New;
    return std::nullopt;
}

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
    EDecorrelation Decorrelation = EDecorrelation::Legacy;
};

void ApplyPlanPasses(TOperatorPtr& plan, TPlanPassOptions options = {});

} // namespace NQdb
