#pragma once

#include <qdb/plan/ops/operator.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NQdb {

// Built once, shared by every reference, optimized as its own root. Plan may
// itself contain TCteRef (nested CTEs form a DAG).
struct TCteDefinition {
    uint32_t Id = 0;
    TOperatorPtr Plan;
    NQumir::NAst::TTypePtr OutputType; // canonical struct schema, bare names
};
using TCteDefinitionPtr = std::shared_ptr<TCteDefinition>;

// Opaque leaf carrying the definition's canonical schema; per-reference aliasing
// is the wrapping AliasSubplan project's job. Resolved before lowering.
class TCteRef : public IOperator {
public:
    static constexpr const char* OpId = "cte-ref";

    explicit TCteRef(TCteDefinitionPtr def);

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override { return {}; }
    const TCteDefinitionPtr& Def() const { return Def_; }
    const std::string ToString() const override;

private:
    TCteDefinitionPtr Def_;
};

// Analysis result persisted for the reuse phase.
struct TCteUsageInfo {
    size_t StaticRefCount = 0;
    std::unordered_set<std::string> RequiredOutputs;
};
using TCteUsageMap = std::unordered_map<TCteDefinition*, TCteUsageInfo>;

// Every distinct definition reachable from the plan, transitively through nested
// definitions; dependency-first.
std::vector<TCteDefinitionPtr> CollectCteDefinitions(const TOperatorPtr& plan);

// Assigns query-global ids (dependency-first traversal order).
void AssignCteIds(const TOperatorPtr& plan);

} // namespace NQdb
